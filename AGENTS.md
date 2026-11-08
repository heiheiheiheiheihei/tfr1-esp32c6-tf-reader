# TFR-1 project entry point

Read [README.md](README.md) first; design rationale is in [docs/DESIGN.md](docs/DESIGN.md).

## Commands

```bash
./scripts/flash.sh                 # build + flash /dev/ttyACM0
./scripts/run.sh                   # sudo USB-NBD bridge (Ctrl-C detaches)
./scripts/mount.sh                 # mount /dev/nbd0p1 at /mnt/tfreader
python3 host/tfreader_host.py --info --logs
python3 host/tfreader_host.py --bench 4
```

## Non-negotiables

- ESP32-C6 has **no USB OTG/MSC**. Do not claim a real drive letter; the bridge is NBD.
- LCD and TF share SPI2. Any new SD/FAT access must respect `spi_guard` (or hold `sd_card_lock()`).
- PC attach is exclusive: FATFS is unmounted while NBD is active; detach must remount it.
- Report `Build / Host tests / Device tests / Unverified` separately.
