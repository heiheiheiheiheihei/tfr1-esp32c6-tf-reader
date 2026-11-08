# TFR-1 介绍片 · 三条故事线

素材：实拍 3 张（706×1366）、真机 UI 截图 4 张（172×320）、真机 LIVE 输出
（`--info` / `--bench 4` → 4.00 MiB in 19.20s = 0.21 MiB/s）、真实 NBD 日志串。

事实边界（都来自 README / DESIGN / 真机输出）：
ESP32-C6 无 USB OTG，只能 USB Serial/JTAG；USB CDC 自定义帧 → 主机 NBD → `/dev/nbd0p1`；
顺序读 0.21 MiB/s；16 GB FAT32 mount/umount ≈ 7 s / 37 s；卡由 PC 独占时板端显示
`CARD LOCKED BY PC`。

---

## A · 扇区之旅（object-to-object chain）★推荐

- **装置 / 形式**：主角是一个 512 B 扇区。它不能走 USB MSC（芯片没有 OTG），只能"偷渡"：
  TF 卡 → SPI2 → ESP32-C6 → USB CDC 帧 → 主机 `tfreader_host.py` → NBD → `/dev/nbd0p1` → `mount`。
  每一站都是一个世界，镜头永远跟着这个扇区，每次穿过一层就换一个界面。
- **两句观感**：观众看着一个 512 字节的小方块被拆进 `TFR1` 帧头、穿过 USB 线、在主机端重新拼成一块硬盘；
  松一口气——原来"读卡器"是这么伪造出来的。
- **前两秒**：PC 桌面空着，没有新盘符（"插上了。Windows 不会亮。"），画面一沉，转到板子屏幕亮起 `MEDIA`。
- **节拍**：hook 无盘符 → tension 芯片规格（无 OTG）→ reveal TFR-1 名字 → proof 扇区穿四层（CARD/FILES/LINK 三页 UI 全出现）
  → payoff 真机 `read 4.00 MiB in 19.20s = 0.21 MiB/s` → claim "一时兴起的小玩意，可能没什么实际意义" → end card。
- **视觉**：产品自己的 LCD 语言——深蓝底 `#04101f`、青色 `#35E0FF`、像素网格与扫描线；
  扇区是一枚 512 格的小方块，每格代表 1 字节。
- **声音**：低频脉冲，每穿一层一声短 whoosh，payoff 处一次 riser。

## B · 一块屏幕自己重排（one frame that rebuilds）

- **装置 / 形式**：不换镜头。实拍设备 → 推进到只剩 172×320 的屏 → 屏变成整个世界，
  `MEDIA` 页在画面里重排成 `FILES`、再重排成 `USB LINK`，所有信息块移动、改写、再落位。
- **两句观感**：观众在一个画面里看完整套 UI 的"另一种可能"；炫技，但不太讲"为什么"。
- **前两秒**：深蓝像素从屏幕边缘溢出来，占满画面。
- **节拍**：hook 屏幕溢出 → 三次重排 → 终端从屏幕里长出来 → 数字 → 声明 → end card。
- **视觉**：同上，但完全图形化，实拍只在头尾。
- **声音**：连续 pad，重排时咔哒声对位。

## C · 插上电脑的那一刻（life before and after）

- **装置 / 形式**：最"技术博主"的一条。左边是 PC 端什么都没发生，右边是终端里 `[nbd] attached /dev/nbd0`
  出现；实拍与终端交替，match cut 在青色高亮上。
- **两句观感**：像一条工程记录 vlog，可信度最高，炫酷度最低。
- **前两秒**：`lsblk` 输出静止，没有新设备。
- **节拍**：hook 无盘符 → reveal `run.sh` → `[nbd] attached` → `mount` → `findmnt` → 板端 `ONLINE` → 数字 → 声明 → end card。
- **视觉**：终端为主体，青色光标；实拍做插叙。
- **声音**：键盘声 + 低频；无音乐也能活。

---

取 A，并把 B 的"三页 UI 全展示"折进 proof 段：扇区每穿过一层，板端就切一页
（CARD → FILES → LINK），这一页是 HERO 占比的画面，不是小窗。
