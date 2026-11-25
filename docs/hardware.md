# 微雪 ESP32-S3-Touch-AMOLED-1.75C 硬件资料

[板卡原理图（仓库内 PDF）](hardware/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf) · [微雪资料下载页](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C/Resources-And-Documents) · [原厂示例和出厂镜像](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/tree/0349e331567b3fb33752abc70c9d1ce12f7418e5)

## 天气钟用到的接口

| 部件 | 信号 | 工程中的接法 |
| --- | --- | --- |
| CO5300 圆屏 | QSPI 片选、时钟 | CS = GPIO12，SCK = GPIO38 |
| CO5300 圆屏 | QSPI 数据 | D0、D1、D2、D3 依次为 GPIO4、5、6、7 |
| CO5300 与 CST9217 | 复位 | 共用 GPIO2。固件先探测触控，再初始化屏幕 |
| CST9217 触控 | I²C、触控中断 | SDA = GPIO15，SCL = GPIO14，地址 `0x5A`，INT = GPIO11 |
| AXP2101 电源管理 | I²C | 与触控共用总线，地址 `0x34`，提供电池电量和 POWER 按键事件 |

显示和触控初始化分别见 [clock_face.h](../clock_face.h) 和 [clock_touch.h](../clock_touch.h)。这块样机深睡时由 ULP 读取 AXP2101 的按键状态，检测到 POWER 后唤醒主处理器。[按键处理](../clock_power.h) · [深睡唤醒](../clock_sleep_key.h)

## 样机分区

| 分区 | 起始地址 | 长度 | 用途 |
| --- | --- | --- | --- |
| nvs | `0x9000` | `0x5000` | 保存 Wi-Fi 和显示设置 |
| otadata | `0xE000` | `0x2000` | 应用启动信息 |
| app0 | `0x10000` | `0xC00000` | 天气钟正式应用 |
| ffat | `0xC10000` | `0x3E0000` | FAT 数据区 |
| coredump | `0xFF0000` | `0x10000` | 故障转储 |

随仓库提供的正式镜像只包含应用。升级已采用这一布局的板子时，写入范围和核验方法见[编译与刷写说明](build.md)。[完整分区表](../partitions.csv)

## 电子表盘设计参考

以下是后续设计候选。2026-11-23 正式版内置的七款表盘列在[项目首页](../README.md)。

| 官方参考 | 可参考的布局 |
| --- | --- |
| [卡西欧 G-SHOCK DW-6900TR-1](https://www.casio.com.cn/watches/gshock/product.DW-6900TR-1/) | 上半部三枚圆形液晶窗，下半部显示时、分、秒 |
| [颂拓 Suunto Core All Black](https://us.suunto.com/products/suunto-core-all-black) | 圆形方位刻度，中央大读数与上下两行辅助信息 |
| [佳明 Forerunner 965](https://www.garmin.com.hk/event/2023/forerunner-20th/) | 彩色数显，小时与分钟分别使用空心和实心字形 |
