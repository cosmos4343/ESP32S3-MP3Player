# 实施计划:为 MP3Player 增加 ST7789 SPI 屏显示

## Context(背景)

当前 MP3Player 编译通过但只能通过串口日志查看状态,缺少用户可视界面。要接入一块 ST7789 SPI 7 针屏(cs/dc/rst/sda/scl/vcc/gnd),分辨率 240x320,用来显示播放状态、音量、当前歌曲名、播放进度,以及文件列表浏览。

约束:
- ESP-IDF v6.0 已内置 ST7789 驱动 `esp_lcd_new_panel_st7789`(无需额外下载)
- lvgl 不在内置组件中,需通过 managed_components 引入 `espressif/esp_lvgl_port`(官方维护,封装 lvgl + esp_lcd 集成 + lvgl 任务)
- ESP32-S3 支持 SPI3_HOST(`SOC_SPI_PERIPH_NUM=3`),屏与 SD 卡(SPI2_HOST)走独立总线,避免互斥影响 SD 卡吞吐
- 现有按键 5 个已映射为播放控制,文件列表浏览需要复用 Prev/Next(停止时浏览,播放时切曲)

## 引脚分配

现有占用:GPIO 4-7(SD-SPI2)、10-14(按钮)、15-18(I2S)、19-20(USB-OTG)、22-33(SPI Flash/PSRAM)。

避开上述后,推荐(写入 [board.h](file:///home/chaos/esp/MP3Player/main/config/board.h)):

```c
// ─── LCD (ST7789, SPI3_HOST) ───
#define LCD_SCK_GPIO    8
#define LCD_MOSI_GPIO   9     // 屏无 MISO
#define LCD_CS_GPIO     1
#define LCD_DC_GPIO     2
#define LCD_RST_GPIO    3
#define LCD_BL_GPIO     (-1)  // 7 针屏通常无背光控制脚,VCC 直接供电
#define LCD_SPI_HOST    SPI3_HOST
#define LCD_H_RES       240
#define LCD_V_RES       320
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)  // ST7789 最高 40MHz
```

> 若你的板子 GPIO 1/2/3 已被占用,可改用 38/39/40/41/42,只需要改 board.h。

## 文件改动清单

### 新增

1. **main/display/display.h / display.c** — 屏幕初始化
   - `display_init()`:bus 初始化 → esp_lcd_panel_io_spi → esp_lcd_new_panel_st7789 → esp_lvgl_port 加载
   - `display_get_lvgl_display()`:返回 `lv_display_t*` 给 UI 模块

2. **main/display/ui.h / ui.c** — LVGL UI 创建与刷新
   - `ui_init()`:创建屏幕对象(状态标签、歌曲名、音量条、进度条、文件列表)
   - `ui_update()`:由 player/button 事件触发刷新状态/音量/文件名/进度

### 修改

3. **[main/idf_component.yml](file:///home/chaos/esp/MP3Player/main/idf_component.yml)** — 加依赖
   ```yaml
   dependencies:
     espressif/esp_audio_codec: ">=2.3.0"
     espressif/esp_lvgl_port: "^2"   # 新增
   ```

4. **[main/CMakeLists.txt](file:///home/chaos/esp/MP3Player/main/CMakeLists.txt)** — 注册新目录与源文件
   - `INCLUDE_DIRS` 加 `"display"`
   - `SRCS` 加 `"display/display.c"`, `"display/ui.c"`

5. **[main/config/board.h](file:///home/chaos/esp/MP3Player/main/config/board.h)** — 加 LCD GPIO 宏(如上)

6. **[main/player/player.h](file:///home/chaos/esp/MP3Player/main/player/player.h)** 与 **[main/player/player.c](file:///home/chaos/esp/MP3Player/main/player/player.c)** — 扩展查询接口
   - 新增 `const char *player_get_current_file(void);` — 返回当前播放文件名(basename)
   - 新增 `int player_get_played_sec(void);` — 已播放秒数(player task 中累计,仅 PLAYING 状态累加)
   - 新增 `int player_get_total_sec(void);` — 总时长(无法解析时返回 -1)
   - 在 player.c 加 `static char s_current_file[256]`、`static int s_played_sec`、`static int s_total_sec`,在 `player_set_file` / `player_task` 中更新

7. **[main/storage/file_list.h](file:///home/chaos/esp/MP3Player/main/storage/file_list.h)** — 已有 `file_list_get(i)` / `file_list_count()`,UI 直接复用

8. **[main/input/button.c](file:///home/chaos/esp/Player/main/input/button.c)** — 复用 Prev/Next 在 STOPPED 状态浏览列表
   - 在按钮处理函数里,若 player 状态为 `PLAYER_STATE_STOPPED`,把 BTN_PREV/NEXT 映射为 UI 列表上下移动 + 选择,而不是发 `PLAYER_CMD_PREV/NEXT`
   - Play 键在 STOPPED 时:把当前选中的列表项作为播放目标,通过新增 `player_play_index(size_t idx)` 启动播放

9. **[main/app_main.c](file:///home/chaos/esp/MP3Player/main/app_main.c)** — 接入启动流程
   - 在 `i2s_output_init()` 后加 `display_init()` → `ui_init()`
   - 在 monitor_task 里调用 `ui_update()` 周期刷新(每 500ms)

10. **sdkconfig.defaults**(如不存在则创建) — 加 lvgl/esp_lvgl_port 必要配置
    - `CONFIG_LV_USE_PERF_MONITOR=n`
    - `CONFIG_LV_COLOR_DEPTH_16=y`
    - `CONFIG_LV_FONT_MONTSERRAT_14=y`
    - `CONFIG_LV_MEM_SIZE_KB=64`

## 实施分阶段(便于回滚)

### 阶段 1:屏幕点亮 + 基础显示(最小可行)
- 完成 display.c + ui.c 骨架
- 显示固定字符串 + 状态/音量(只用现有 `player_get_state()` / `player_get_volume()`)
- 不改 player / button / file_list
- 验证:屏幕亮,显示 "MP3 Player" 标题,状态变化时文字刷新

### 阶段 2:歌曲名 + 进度
- 扩展 player 接口(`player_get_current_file`、`player_get_played_sec`)
- UI 加歌曲名标签 + 进度条
- 验证:播放时歌曲名和进度时间实时更新

### 阶段 3:文件列表浏览
- 改 button.c 加 STOPPED 模式下的列表导航
- UI 加 lv_list 控件,填充 `file_list_get(i)`
- 新增 `player_play_index(size_t idx)` 接口启动指定文件
- 验证:停止时按 Prev/Next 上下移动高亮项,Play 启动播放

## 验证方式

1. `idf.py build` 编译通过
2. `idf.py flash monitor` 烧录并看串口
3. 屏幕亮起,启动后显示 "MP3 Player" 标题
4. 按 Play 后状态变化为 ▶,歌曲名出现,进度条开始走
5. 按 Vol+/- 音量条变化
6. 停止时按 Prev/Next 浏览列表,Play 选定播放

## 关键依赖版本

- `espressif/esp_lvgl_port: "^2"` — 兼容 ESP-IDF v6.0 + lvgl v9
- 内置 `esp_lcd`(`esp_lcd_new_panel_st7789`、`esp_lcd_new_panel_io_spi`)
- 内置 `esp_driver_spi`(`spi_bus_initialize`)
