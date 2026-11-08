#!/usr/bin/env python3
"""TFR-1 host bridge: USB serial <-> Linux NBD block device.

The ESP32-C6 has no USB OTG, so it cannot present a real USB Mass Storage
device.  This host process speaks a small framed block protocol over the
board's USB Serial/JTAG CDC interface and hands the block device to the Linux
NBD kernel driver.  Run it as root, then mount /dev/nbdXp1.
"""
from __future__ import annotations

import argparse
import errno
import fcntl
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    serial = None

MAGIC = 0x31524654
HDR = struct.Struct("<IBBHIH")
CRC_INIT = 0xFFFF

REQ_HELLO = 0x01
REQ_INFO = 0x02
REQ_READ = 0x10
REQ_WRITE = 0x11
REQ_FLUSH = 0x12
REQ_HOST = 0x13
REQ_PING = 0x14
REQ_STATUS = 0x20
REQ_LOG = 0x21
REQ_ECHO = 0x30
REQ_SCREEN = 0x40
REQ_SCREEN_CHUNK = 0x41
REQ_UI_PAGE = 0x42
REQ_SCREEN_END = 0x43

RESP_HELLO = 0x81
RESP_INFO = 0x82
RESP_READ = 0x90
RESP_WRITE = 0x91
RESP_FLUSH = 0x92
RESP_HOST = 0x93
RESP_PING = 0x94
RESP_STATUS = 0xA0
RESP_LOG = 0xA1
RESP_ECHO = 0xB0
RESP_SCREEN = 0xC0
RESP_SCREEN_CHUNK = 0xC1
RESP_UI_PAGE = 0xC2
RESP_SCREEN_END = 0xC3

FLAG_ERROR = 0x01

NBDMAGIC = b"NBDMAGIC"
IHAVEOPT = b"IHAVEOPT"
NBD_OPT_EXPORT_NAME = 1
NBD_FLAG_FIXED_NEWSTYLE = 1
NBD_FLAG_NO_ZEROES = 2
NBD_FLAG_C_FIXED_NEWSTYLE = 1
NBD_FLAG_C_NO_ZEROES = 2
NBD_FLAG_HAS_FLAGS = 1
NBD_FLAG_SEND_FLUSH = 4
NBD_FLAG_SEND_FUA = 8
NBD_FLAG_SEND_TRIM = 32

NBD_CMD_READ = 0
NBD_CMD_WRITE = 1
NBD_CMD_DISC = 2
NBD_CMD_FLUSH = 3
NBD_CMD_TRIM = 4
NBD_REQUEST_MAGIC = 0x25609513
NBD_REPLY_MAGIC = 0x67446698

NBD_SET_SOCK = 0xAB00
NBD_SET_BLKSIZE = 0xAB01
NBD_SET_SIZE = 0xAB02
NBD_DO_IT = 0xAB03
NBD_CLEAR_SOCK = 0xAB04
NBD_CLEAR_QUE = 0xAB05
NBD_SET_SIZE_BLOCKS = 0xAB07
NBD_DISCONNECT = 0xAB08
NBD_SET_TIMEOUT = 0xAB09
NBD_SET_FLAGS = 0xAB0A


def crc16(data: bytes) -> int:
    crc = CRC_INIT
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class LinkError(RuntimeError):
    pass


class SerialLink:
    def __init__(self, port: str, timeout: float = 2.0):
        if serial is None:
            raise LinkError("pyserial is required: python3 -m pip install pyserial")
        self.seq = 0
        self.timeout = timeout
        self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.reset_input_buffer()
        self._rx = bytearray()

    def close(self) -> None:
        self.ser.close()

    def _read_exact(self, n: int, timeout: float | None = None) -> bytes:
        end = time.monotonic() + (self.timeout if timeout is None else timeout)
        while len(self._rx) < n:
            chunk = self.ser.read(max(4096, n - len(self._rx)))
            if chunk:
                self._rx += chunk
            elif time.monotonic() > end:
                raise LinkError(f"serial timeout: wanted {n}, got {len(self._rx)}")
        out = bytes(self._rx[:n])
        del self._rx[:n]
        return out

    def _wait_magic(self, timeout: float) -> None:
        end = time.monotonic() + timeout
        while True:
            idx = self._rx.find(b"TFR1")
            if idx >= 0:
                if idx:
                    del self._rx[:idx]
                if len(self._rx) >= 14:
                    return
            chunk = self.ser.read(4096)
            if chunk:
                self._rx += chunk
            elif time.monotonic() > end:
                raise LinkError("response timeout")
    def _send(self, typ: int, payload: bytes = b"", flags: int = 0) -> int:
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq
        head = struct.pack("<BBHI", typ, flags, seq, len(payload))
        crc = crc16(head + payload)
        self.ser.write(HDR.pack(MAGIC, typ, flags, seq, len(payload), crc) + payload)
        self.ser.flush()
        return seq

    def _recv(self, want_seq: int) -> tuple[int, int, bytes]:
        self._wait_magic(self.timeout)
        raw = self._read_exact(14, self.timeout)
        magic, typ, flags, seq, length, crc = HDR.unpack(raw)
        if magic != MAGIC:
            raise LinkError(f"bad frame magic 0x{magic:08x}")
        payload = self._read_exact(length, self.timeout)
        if crc16(raw[4:12] + payload) != crc:
            raise LinkError("frame CRC mismatch")
        if seq != want_seq:
            return self._recv(want_seq)
        if flags & FLAG_ERROR:
            code = struct.unpack("<i", payload[:4])[0] if len(payload) >= 4 else -1
            raise LinkError(f"device error {code}")
        return typ, flags, payload

    def request(self, typ: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        if timeout is not None:
            old, self.timeout = self.timeout, timeout
        try:
            seq = self._send(typ, payload)
            rtyp, _flags, data = self._recv(seq)
            return data
        finally:
            if timeout is not None:
                self.timeout = old

    def hello(self) -> dict:
        data = self.request(REQ_HELLO)
        protocol, sector_size, max_transfer, flags, sectors = struct.unpack("<IIIIQ", data[:24])
        return {
            "protocol": protocol,
            "sector_size": sector_size,
            "max_transfer": max_transfer,
            "flags": flags,
            "sectors": sectors,
            "size": sectors * sector_size,
        }

    def info(self) -> dict:
        data = self.request(REQ_INFO)
        fmt = "<IIIQQQIIIIIII9s8s16s"
        vals = struct.unpack(fmt, data[:struct.calcsize(fmt)])
        keys = ["present", "mounted", "host", "capacity", "total", "free",
                "sector_size", "sector_count", "mfg", "oem", "serial", "year",
                "month", "product", "fs", "label"]
        out = dict(zip(keys, vals))
        out["product"] = out["product"].split(b"\0", 1)[0].decode("ascii", "replace")
        out["fs"] = out["fs"].split(b"\0", 1)[0].decode("ascii", "replace")
        out["label"] = out["label"].split(b"\0", 1)[0].decode("ascii", "replace")
        # Appended after the original 97-byte layout; older firmware omits it.
        out["fault"] = data[97] if len(data) > 97 else 0
        return out

    def host(self, enable: bool) -> None:
        self.request(REQ_HOST, bytes([1 if enable else 0]), timeout=5.0)

    def ping(self) -> None:
        self.request(REQ_PING, timeout=2.0)

    def read_sectors(self, lba: int, count: int) -> bytes:
        data = self.request(REQ_READ, struct.pack("<QI", lba, count), timeout=5.0)
        if len(data) != count * 512:
            raise LinkError(f"short read: {len(data)} != {count * 512}")
        return data

    def write_sectors(self, lba: int, data: bytes) -> None:
        if len(data) % 512:
            raise LinkError("write length must be sector aligned")
        count = len(data) // 512
        self.request(REQ_WRITE, struct.pack("<QI", lba, count) + data, timeout=8.0)

    def flush(self) -> None:
        self.request(REQ_FLUSH, timeout=3.0)

    def status(self) -> dict:
        data = self.request(REQ_STATUS)
        fmt = "<QQQQIIIII"
        vals = struct.unpack(fmt, data[:struct.calcsize(fmt)])
        return dict(zip(["read_bytes", "write_bytes", "read_ops", "write_ops",
                         "errors", "host_active", "card_present", "free_heap",
                         "min_heap", "tx_errors"], vals))

    def log(self) -> str:
        return self.request(REQ_LOG, timeout=3.0).decode("utf-8", "replace")

    def echo(self, data: bytes) -> bytes:
        return self.request(REQ_ECHO, data, timeout=5.0)

    def ui_page(self, page: int) -> None:
        self.request(REQ_UI_PAGE, bytes([page & 0xFF]), timeout=3.0)

    def screenshot_begin(self) -> tuple[int, int, int]:
        data = self.request(REQ_SCREEN, timeout=10.0)
        w, h, total, fmt = struct.unpack("<IIII", data[:16])
        if fmt != 1 or total == 0:
            raise LinkError("device returned an unsupported screenshot")
        return w, h, total

    def screenshot_chunk(self, offset: int, length: int) -> bytes:
        return self.request(REQ_SCREEN_CHUNK, struct.pack("<II", offset, length), timeout=10.0)

    def screenshot_end(self) -> None:
        self.request(REQ_SCREEN_END, timeout=3.0)


def fmt_bytes(n: int) -> str:
    if n >= 1 << 30:
        return f"{n / (1 << 30):.2f} GiB"
    if n >= 1 << 20:
        return f"{n / (1 << 20):.1f} MiB"
    if n >= 1 << 10:
        return f"{n / (1 << 10):.1f} KiB"
    return f"{n} B"


def save_rgb565_png(path: str, w: int, h: int, data: bytes) -> None:
    import zlib
    import binascii

    need = w * h * 2
    if len(data) < need:
        raise LinkError(f"short screenshot: {len(data)} < {need}")
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        base = y * w * 2
        for x in range(w):
            v = data[base + x * 2] | (data[base + x * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            raw += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", binascii.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def capture_screen(link: SerialLink, path: str) -> None:
    # Let the boot splash hand over to the dashboard.
    time.sleep(1.8)
    w, h, total = link.screenshot_begin()
    out = bytearray()
    while len(out) < total:
        n = min(16384, total - len(out))
        chunk = link.screenshot_chunk(len(out), n)
        if len(chunk) != n:
            raise LinkError(f"short screenshot chunk: {len(chunk)} != {n}")
        out += chunk
    save_rgb565_png(path, w, h, bytes(out))
    link.screenshot_end()
    print(f"[screen] {w}x{h} -> {path}")


class TFRNbdServer:
    def __init__(self, link: SerialLink, size: int, sectors: int):
        self.link = link
        self.size = size
        self.sectors = sectors
        self.stop = False
        self.listener: socket.socket | None = None
        self.conn: socket.socket | None = None
        self.port = 0
        self.transmission_flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_FUA

    def start(self) -> int:
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        return self.port

    def handshake(self, client: socket.socket) -> socket.socket:
        assert self.listener is not None
        self.listener.settimeout(10.0)
        self.conn, _ = self.listener.accept()
        self.conn.settimeout(10.0)
        client.settimeout(10.0)

        def recv_sock(sock, n):
            out = bytearray()
            while len(out) < n:
                chunk = sock.recv(n - len(out))
                if not chunk:
                    raise LinkError("NBD negotiation disconnected")
                out += chunk
            return bytes(out)

        # server -> client greeting
        greeting = NBDMAGIC + IHAVEOPT + struct.pack(">H", NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES)
        self.conn.sendall(greeting)
        if recv_sock(client, len(greeting)) != greeting:
            raise LinkError("bad NBD greeting")
        # client -> server flags + EXPORT_NAME
        cflags = NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES
        client.sendall(struct.pack(">I", cflags))
        client.sendall(IHAVEOPT + struct.pack(">II", NBD_OPT_EXPORT_NAME, 0))
        # server reads them
        sflags = struct.unpack(">I", recv_sock(self.conn, 4))[0]
        if not (sflags & NBD_FLAG_C_FIXED_NEWSTYLE):
            raise LinkError("NBD client did not negotiate fixed newstyle")
        if recv_sock(self.conn, 8) != IHAVEOPT:
            raise LinkError("bad NBD option magic")
        opt, length = struct.unpack(">II", recv_sock(self.conn, 8))
        if opt != NBD_OPT_EXPORT_NAME or length != 0:
            raise LinkError(f"unsupported NBD option {opt}")
        # server -> client export size + transmission flags
        self.conn.sendall(struct.pack(">QH", self.size, self.transmission_flags))
        if not (sflags & NBD_FLAG_C_NO_ZEROES):
            self.conn.sendall(b"\0" * 124)
        # client consumes the export reply
        recv_sock(client, 10)
        if not (sflags & NBD_FLAG_C_NO_ZEROES):
            recv_sock(client, 124)
        self.conn.settimeout(None)
        client.settimeout(None)
        return self.conn

    def _recv_exact(self, n: int) -> bytes:
        assert self.conn is not None
        out = bytearray()
        while len(out) < n:
            chunk = self.conn.recv(n - len(out))
            if not chunk:
                raise LinkError("NBD client disconnected during negotiation")
            out += chunk
        return bytes(out)

    def serve(self) -> None:
        assert self.conn is not None
        self.conn.settimeout(None)
        last_ping = time.monotonic()
        while not self.stop:
            try:
                ready, _, _ = select.select([self.conn], [], [], 0.5)
                if not ready:
                    if time.monotonic() - last_ping > 2.0:
                        self.link.ping()
                        last_ping = time.monotonic()
                    continue
                raw = self._recv_exact(28)
                last_ping = time.monotonic()
            except (ConnectionError, LinkError, OSError) as e:
                print(f"[nbd] session ended: {e}", file=sys.stderr)
                break
            magic, flags, cmd, handle, offset, length = struct.unpack(">IHHQQI", raw)
            if magic != NBD_REQUEST_MAGIC:
                break
            if os.environ.get("TFR_VERBOSE"):
                print(f"[nbd] cmd={cmd} off={offset} len={length}", flush=True)
            err = 0
            data = b""
            try:
                if cmd == NBD_CMD_READ:
                    data = self._read(offset, length)
                elif cmd == NBD_CMD_WRITE:
                    try:
                        payload = self._recv_exact(length)
                    except (ConnectionError, LinkError, OSError) as e:
                        print(f"[nbd] client gone during write: {e}", file=sys.stderr)
                        break
                    self._write(offset, payload)
                elif cmd == NBD_CMD_FLUSH:
                    self.link.flush()
                elif cmd == NBD_CMD_DISC:
                    break
                elif cmd == NBD_CMD_TRIM:
                    err = errno.EINVAL
                else:
                    err = errno.EINVAL
            except LinkError as e:
                print(f"[nbd] I/O error: {e}", file=sys.stderr)
                err = errno.EIO
            try:
                self.conn.sendall(struct.pack(">IIQ", NBD_REPLY_MAGIC, err, handle) + (data if cmd == NBD_CMD_READ and err == 0 else b""))
            except (ConnectionError, OSError):
                break

    def _read(self, offset: int, length: int) -> bytes:
        if offset % 512 or length % 512:
            raise LinkError("unaligned NBD read")
        if offset + length > self.size:
            raise LinkError("NBD read past end")
        out = bytearray()
        lba = offset // 512
        left = length // 512
        while left:
            count = min(32, left)
            out += self.link.read_sectors(lba, count)
            lba += count
            left -= count
        return bytes(out)

    def _write(self, offset: int, data: bytes) -> None:
        if offset % 512 or len(data) % 512:
            raise LinkError("unaligned NBD write")
        if offset + len(data) > self.size:
            raise LinkError("NBD write past end")
        lba = offset // 512
        for i in range(0, len(data), 16384):
            chunk = data[i:i + 16384]
            self.link.write_sectors(lba + i // 512, chunk)
        self.link.flush()

    def close(self) -> None:
        self.stop = True
        for s in (self.conn, self.listener):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass


def find_nbd_device() -> str:
    for i in range(16):
        path = f"/dev/nbd{i}"
        pid_path = f"/sys/block/nbd{i}/pid"
        if not os.path.exists(path):
            continue
        try:
            with open(pid_path) as f:
                pid = f.read().strip()
            if pid and pid != "0":
                continue
        except OSError:
            pass
        return path
    raise LinkError("no free /dev/nbdX (try: sudo modprobe nbd max_part=8)")


def ensure_nbd_module() -> None:
    if os.path.exists("/dev/nbd0"):
        return
    try:
        subprocess.run(["modprobe", "nbd", "max_part=8"], check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        raise LinkError(f"cannot load nbd module: {e}") from e
    time.sleep(0.2)


def attach_nbd(link: SerialLink, server: TFRNbdServer, dev_path: str) -> tuple[int, int]:
    if os.geteuid() != 0:
        raise LinkError("NBD attach needs root: run with sudo")
    ensure_nbd_module()
    server.start()
    client = socket.create_connection(("127.0.0.1", server.port), timeout=5.0)
    conn = server.handshake(client)

    fd = os.open(dev_path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, NBD_CLEAR_SOCK)
        fcntl.ioctl(fd, NBD_CLEAR_QUE)
        fcntl.ioctl(fd, NBD_SET_BLKSIZE, 512)
        fcntl.ioctl(fd, NBD_SET_SIZE_BLOCKS, server.size // 512)
        fcntl.ioctl(fd, NBD_SET_FLAGS, server.transmission_flags)
        fcntl.ioctl(fd, NBD_SET_SOCK, client.fileno())
        pid = os.fork()
        if pid == 0:
            try:
                server.close()
                client.close()
                fcntl.ioctl(fd, NBD_DO_IT)
            finally:
                os._exit(0)
        client.close()
        print(f"[nbd] attached {dev_path}  {fmt_bytes(server.size)}  pid={pid}")
        return fd, pid
    except Exception:
        os.close(fd)
        raise


def detach_nbd(fd: int, pid: int, dev_path: str) -> None:
    try:
        fcntl.ioctl(fd, NBD_DISCONNECT)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    try:
        fcntl.ioctl(fd, NBD_CLEAR_SOCK)
    except OSError:
        pass
    try:
        os.close(fd)
    except OSError:
        pass
    print(f"[nbd] detached {dev_path}")


def print_info(link: SerialLink) -> None:
    h = link.hello()
    i = link.info()
    print("TFR-1  protocol", h["protocol"])
    print("card   ", "present" if i["present"] else "absent",
          "|", "mounted" if i["mounted"] else "unmounted",
          "| host" if i["host"] else "")
    if i["present"]:
        print(f"model  {i['product']}   serial 0x{i['serial']:08X}   {i['year']}-{i['month']:02d}")
        print(f"size   {fmt_bytes(i['capacity'])} ({i['sector_count']} x {i['sector_size']} B)")
        fs = i["fs"] if i["mounted"] else f"{i['fs'] or 'RAW'} (unmounted)"
        print(f"fs     {fs}   free {fmt_bytes(i['free'])} / {fmt_bytes(i['total'])}")
        if i["fault"]:
            print("fault  block-level self-test FAILED: distant LBAs read back "
                  "identical data; card is unusable (fake/defective)")


def bench(link: SerialLink, mib: int) -> None:
    h = link.hello()
    sectors = min(h["sectors"], (mib * 1024 * 1024) // 512)
    chunk = 32
    link.host(True)
    try:
        start = time.monotonic()
        done = 0
        while done < sectors:
            n = min(chunk, sectors - done)
            link.read_sectors(done, n)
            done += n
            if done % (chunk * 64) == 0:
                print(f"  {done * 512 / (1 << 20):.1f} MiB", flush=True)
        dt = time.monotonic() - start
        print(f"read {done * 512 / (1 << 20):.2f} MiB in {dt:.2f}s = {done * 512 / dt / (1 << 20):.2f} MiB/s")
    finally:
        try:
            link.host(False)
            print(link.log().strip())
        except Exception as e:
            print(f"cleanup: {e}", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description="TFR-1 USB-NBD TF card reader host bridge")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--attach", action="store_true", help="attach the card as a Linux NBD block device (root)")
    ap.add_argument("--device", default=None, help="NBD device, default auto")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--logs", action="store_true")
    ap.add_argument("--bench", type=int, metavar="MIB")
    ap.add_argument("--screenshot", metavar="PATH", help="capture the LCD as PNG")
    ap.add_argument("--ui-page", type=int, choices=(0, 1, 2), help="switch UI page before screenshot")
    args = ap.parse_args()

    link = SerialLink(args.port)
    try:
        # The CDC interface may enumerate a moment after reset.
        last = None
        for _ in range(20):
            try:
                link.hello()
                break
            except LinkError as e:
                last = e
                time.sleep(0.25)
        else:
            raise LinkError(f"device not answering: {last}")

        if args.ui_page is not None:
            link.ui_page(args.ui_page)
            time.sleep(0.4)

        if args.info or not args.attach:
            print_info(link)
        if args.logs:
            print("--- device log ---")
            print(link.log().strip())
        if args.bench:
            bench(link, args.bench)
        if not args.attach:
            if args.screenshot:
                capture_screen(link, args.screenshot)
            return 0

        info = link.info()
        if not info["present"]:
            raise LinkError("no TF card inserted")
        if info.get("fault"):
            # Every LBA aliases to one block: exporting it would silently
            # corrupt anything the PC writes, and no data can be recovered.
            raise LinkError(
                "card data path is broken (block-level self-test failed: distant "
                "LBAs read back identical data); refusing to export it over NBD")
        if not info["mounted"]:
            print(f"note: {info['fs'] or 'unknown'} filesystem on board; "
                  "exporting raw blocks for the PC to mount", file=sys.stderr)
        h = link.hello()
        if h["sectors"] == 0:
            raise LinkError("device reports zero sectors")
        link.host(True)
        ensure_nbd_module()
        server = TFRNbdServer(link, h["size"], h["sectors"])
        dev = args.device or find_nbd_device()
        fd, pid = attach_nbd(link, server, dev)
        print(f"[nbd] export {dev}  sector size {h['sector_size']}")
        if args.screenshot:
            capture_screen(link, args.screenshot)
        print(f"[nbd] next: sudo partprobe {dev} && lsblk {dev}")
        print(f"[nbd] mount: sudo mount {dev}p1 /mnt/tfreader   (or {dev} for a superfloppy)")
        stop = {"v": False}

        def on_signal(signum, frame):
            stop["v"] = True
            try:
                if server.conn is not None:
                    server.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

        signal.signal(signal.SIGINT, on_signal)
        signal.signal(signal.SIGTERM, on_signal)
        try:
            server.serve()
        finally:
            server.close()
            detach_nbd(fd, pid, dev)
            try:
                link.host(False)
            except Exception:
                pass
        return 0
    finally:
        link.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LinkError as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
