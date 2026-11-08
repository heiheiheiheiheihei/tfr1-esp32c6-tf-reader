# TFR-1 设计记录

## 1. 问题与约束

- 板：Waveshare ESP32-C6-LCD-1.47，ESP32-C6FH4，4 MB Flash，512 KB SRAM，无 PSRAM。
- 屏：ST7789T，172×320，SPI2（MOSI 6 / SCLK 7 / CS 14 / DC 15 / RST 21 / BL 22）。
- TF：SPI 模式，CS 4，与 LCD 共用 MOSI/SCLK；无 CD 引脚。
- USB：仅 USB Serial/JTAG（CDC-ACM + JTAG），**无 OTG/MSC**。

## 2. Key Judgment

**不做假的 USB MSC，也不做只能拷文件的 FUSE 文件夹；用 USB CDC 传输原始 512 B 块，主机端 NBD 暴露整卡。**

理由：

- 保留“块设备”语义：PC 可 `mount`、`mkfs`、`fsck`、分区，行为最接近读卡器；
- 避免 FATFS 缓存与 PC 裸写并发导致文件系统损坏：PC attach 时板端**卸载 FATFS**，只保留 `sdmmc_card_t` 原始访问；
- 主机端只依赖 Linux 自带 NBD，不需要额外内核驱动。

## 3. Avoided Misfix

- **不伪造 MSC 描述符**：C6 的 USB Serial/JTAG 外设描述符固定，无法枚举成 Mass Storage；改 descriptor 只会得到不能用的设备。
- **不做 Wi-Fi/WebDAV 替代**：多一层网络配置，且不是“USB 读卡器”。
- **不在 FATFS 挂载状态下让 PC 裸写**：FATFS 会缓存 FAT/目录扇区，PC 写入后板端缓存失效，轻则列表陈旧，重则覆盖数据。
- **不使用全屏 framebuffer**：172×320×2 = 110 KB，512 KB SRAM 下不划算；LVGL 用 2×40 行局部缓冲。

## 4. 数据通路

```
PC file system
   ↓ mount /dev/nbd0p1
Linux NBD kernel driver
   ↓ NBD protocol (localhost TCP)
tfreader_host.py  ── USB CDC 帧协议 ──  ESP32-C6
                                          ├─ sdmmc_read/write_sectors
                                          └─ LVGL UI / RGB
```

### USB 帧

```
magic "TFR1" | type u8 | flags u8 | seq u16 | len u32 | crc16 u16 | payload
crc16-CCITT-FALSE over type..len + payload
```

命令：HELLO、INFO、READ、WRITE、FLUSH、HOST、PING、STATUS、LOG、ECHO、SCREEN、UI_PAGE。
READ/WRITE 的 LBA 为 512 B 扇区；单帧最多 16 KiB。

### 主机模式状态机

```
FAT mounted --HOST(1)--> unmount FAT, raw card, UI "ONLINE"
raw card --HOST(0)/8 s no ping/USB gone--> mount FAT, UI "READY"
```

主机每 2 s 发 PING；NBD 空闲时也发，避免 8 s 超时误释放。

## 5. 共享 SPI 的 Root Cause

LCD 和 TF 共用 SPI2。LVGL 的 `esp_lcd_panel_draw_bitmap()` 是异步的；SDSPI 的 `poll_busy()` 用 `spi_device_polling_transmit()` 轮询。两者在特定时序下同时操作 SPI 外设，触发：

```
assert failed: spi_hal_setup_trans
(spi_ll_get_running_cmd(hw) == 0)
```

core dump 的崩溃任务为 `usb_link`，栈顶在 `sdspi_host_start_command -> poll_busy`。

**修复**：应用层 `spi_guard` 二值信号量。LCD flush 前获取，`on_color_trans_done` ISR 释放；所有 SD 事务（mount/check/read/write/FAT 扫描）获取同一信号量。这样把“两个设备各自的驱动互斥”提升为“整条共享总线互斥”。

**验证**：修复前 8 MiB 直读在约 0.2–4 MiB 处 panic；修复后 8 MiB 通过，NBD 4 MiB 读 + 128 KiB 写通过。

## 6. 资源与性能

- LVGL 内存池 64 KB；绘制缓冲 2×172×40×2 ≈ 27 KB；截图缓冲 110 KB 按需分配、用完释放。
- USB TX/RX 各 32 KiB；协议 payload 32 KiB；块缓冲 16 KiB。
- 实测空闲堆约 151 KB。
- 瓶颈是 USB Serial/JTAG 全速 CDC + 每帧应答，不是 SD（20 MHz）。实测 0.20–0.25 MB/s。
- 16 GB FAT32 的 mount/umount 会触发约 10 MB FAT 元数据读取；在 0.25 MB/s 下表现为约 7 s / 37 s，属于带宽限制，不是 NBD 死锁（`TFR_VERBOSE=1` 可看到 256 KiB 顺序读请求）。
- 每次 `send_frame` 后调用 `usb_serial_jtag_wait_tx_done()`；不等待会出现约 2 MiB 后帧尾丢失/主机超时。

## 7. 后续可做

- 文件级 FUSE 模式（不锁卡，但不能格式化）；
- exFAT 支持与大容量卡验证；
- NBD 多队列/批量读，提升吞吐；
- 把 8 s 心跳改为 USB SOF 连接状态 + 显式 detach；
- 外壳与磁吸/支架。
