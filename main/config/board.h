#pragma once

// ─── SPI (TF Card) ───
// 硬件接线:SCK/MOSI/MISO/CS 接下表 GPIO(3.3V 电平);
// 模块 VCC 必须接开发板 5V —— 本模块自带稳压与电平转换,
// 接 3.3V 时卡会出现 CMD0/8 正常但 ACMD41 永不完成的欠压症状。
#define SD_CS_GPIO      4
#define SD_SCK_GPIO     5
#define SD_MOSI_GPIO    6
#define SD_MISO_GPIO    7
#define SD_SPI_HOST     SPI2_HOST

// ─── I2S (MAX98357A) ───
#define I2S_BCLK_GPIO   15
#define I2S_LRC_GPIO    16
#define I2S_DIN_GPIO    17
#define I2S_GAIN_GPIO   18
#define I2S_PORT        I2S_NUM_0

// ─── Buttons ───
#define BTN_PLAY_GPIO   10
#define BTN_PREV_GPIO   11
#define BTN_NEXT_GPIO   12
#define BTN_VOLP_GPIO   13
#define BTN_VOLM_GPIO   14
#define BTN_COUNT       5

// ─── Power ───
// (nothing for now)

// ─── LCD (ST7789, SPI3_HOST, 240x320) ───
#define LCD_SCK_GPIO    8
#define LCD_MOSI_GPIO   9     // 屏无 MISO
#define LCD_CS_GPIO     1
#define LCD_DC_GPIO     2
#define LCD_RST_GPIO    3
#define LCD_BL_GPIO     (-1)  // 7 针屏通常无背光控制脚,VCC 直接供电
#define LCD_SPI_HOST    SPI3_HOST
#define LCD_H_RES       240
#define LCD_V_RES       320
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)  // ST7789 最高 40MHz;若出现花屏可降到 20MHz
