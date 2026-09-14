# ESP32-S3 MP3 Player

基于 ESP32-S3 的便携式 MP3 播放器：从 SD 卡读取 MP3 文件，硬件解码后通过 I2S 功放输出，配有 2.4 寸 SPI 彩屏和 LVGL 图形界面，支持 5 个实体按键操作与中文文件名显示。

## 功能特性

- **MP3 硬解码**：基于 `esp_audio_codec`，支持任意采样率/单声道立体声自动适配（首帧解码后动态重配 I2S，单声道自动上混为立体声）
- **稳定播放**：解码输入分片喂入 + 未消费字节 holdover 保留，16×240 帧 I2S DMA 缓冲（约 87ms），实测播放速率偏差 0.003%、零 underrun
- **SD 卡存储**：SDSPI + FATFS，递归扫描整卡 MP3，播放列表最多 2048 首
- **中文文件名**：FATFS UTF-8 API + Noto Sans SC 16px 字体，GBK 编码的中文文件名正常显示
- **LVGL 界面**：240×320 ST7789 彩屏，文件列表浏览、曲目信息、播放进度条与时间显示
- **5 键操控**：播放/暂停、上一曲、下一曲、音量加、音量减（支持长按连发调音量）
- **容错启动**：显示/UI 先初始化，SD 卡缺失或异常时不阻断启动，系统照常运行

## 硬件平台

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-S3（16MB Flash + 8MB 八线 PSRAM） |
| 显示屏 | ST7789 240×320 SPI 7 针屏（无 MISO、无背光脚），SPI 40MHz |
| 音频 | MAX98357A I2S D 类功放（5V 供电） |
| 存储 | Micro SD 卡模块（自带稳压+电平转换），FAT32 |
| 按键 | 5 个轻触按键，低电平有效（另一端接 GND，内部上拉） |

### 接线表

| 模块 | 信号 | GPIO | 备注 |
| --- | --- | --- | --- |
| SD 卡 | CS | 4 | **模块 VCC 必须接 5V**（3.3V 下 ACMD41 无法完成） |
| SD 卡 | SCK | 5 | |
| SD 卡 | MOSI | 6 | |
| SD 卡 | MISO | 7 | |
| LCD | SCK | 8 | |
| LCD | MOSI | 9 | 屏无 MISO |
| LCD | CS | 1 | |
| LCD | DC | 2 | |
| LCD | RST | 3 | |
| 按键 | 播放/暂停 | 10 | |
| 按键 | 上一曲 | 11 | |
| 按键 | 下一曲 | 12 | |
| 按键 | 音量 + | 13 | 长按连发 |
| 按键 | 音量 − | 14 | 长按连发 |
| MAX98357A | BCLK | 15 | VIN 接 5V |
| MAX98357A | LRC (WS) | 16 | |
| MAX98357A | DIN | 17 | |

> 引脚定义集中在 [`main/config/board.h`](main/config/board.h)，改线只需修改该文件。

## 软件环境

- **ESP-IDF v6.0**
- 托管组件（构建时自动下载，无需手工声明内置组件）：
  - `espressif/esp_audio_codec >= 2.3.0`
  - `espressif/esp_lvgl_port ^2`（含 LVGL）
- 分区表：自定义 [`partitions.csv`](partitions.csv)，factory 应用分区 4MB

## 编译与烧录

```bash
# 1. 设置目标芯片（只需一次）
idf.py set-target esp32s3

# 2. 编译
idf.py build

# 3. 烧录并监视（USB 串口，本机为 /dev/ttyACM0）
idf.py -p /dev/ttyACM0 flash monitor
```

> Flash 大小需配置为 16MB（见 `sdkconfig.defaults` 中的 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB`）。
> 进入下载模式：按住 BOOT，短按 RST，继续保持 BOOT 约 1 秒后松开。

## 使用方法

1. 将 MP3 文件拷入 SD 卡（根目录或 `/music` 目录均可，支持子目录递归扫描），插入卡座
2. 上电后屏幕显示文件列表，用上一曲/下一曲键选择曲目
3. 按播放/暂停键开始播放，界面显示进度条与已播放时间
4. 音量 ± 键调节音量（支持长按），播放中按上/下一曲切换曲目

## 已知硬件相关说明（重要）

本项目在如下非标准硬件条件下做了适配，换卡/换模块时需注意：

1. **SD 卡模块必须 5V 供电**。该模块虽带稳压与电平转换，但在 3.3V 供电时卡能应答 CMD0/CMD8，ACMD41 却永不完成（欠压症状）。
2. **部分老 SDSC 卡（如 SD512）拒绝 CMD59（CRC 开关）**，返回 illegal command。需对 ESP-IDF 安装目录打两个小补丁（升级/重装 IDF 后会丢失，需重新打）：
   - `components/sdmmc/sdmmc_sd.c` 的 `sdmmc_init_spi_crc()`：容忍 `ESP_ERR_NOT_SUPPORTED`
   - `components/sdmmc/sdmmc_common.h`：`MAX_ERRORS` 由 3 提高到 5

## 目录结构

```
├── main/
│   ├── app_main.c            # 启动入口与任务编排
│   ├── audio/                # MP3 解码、I2S 输出
│   ├── config/               # 板级引脚 (board.h) 与全局参数 (app_config.h)
│   ├── display/              # ST7789 驱动、LVGL UI、中文字体
│   ├── input/                # 按键扫描（消抖/长按）
│   ├── player/               # 播放核心与环形缓冲
│   ├── power/                # 电源初始化
│   └── storage/              # SD 卡挂载、文件列表
├── partitions.csv            # 自定义分区表
├── sdkconfig.defaults        # 默认配置（PSRAM、16MB Flash、FATFS UTF-8 等）
└── CMakeLists.txt
```
