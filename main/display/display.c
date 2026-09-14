#include "display.h"
#include "board.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISPLAY";

static display_handles_t s_handles;
static lv_display_t *s_lv_disp = NULL;

esp_err_t display_init(void)
{
    /* 1. Initialize SPI3 bus for the LCD (only MOSI + SCK; no MISO needed). */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_SCK_GPIO,
        .mosi_io_num     = LCD_MOSI_GPIO,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Create LCD panel IO on the SPI bus. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS_GPIO,
        .dc_gpio_num       = LCD_DC_GPIO,
        .spi_mode          = 0,
        .pclk_hz           = LCD_SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .flags             = {
            .dc_high_on_cmd  = 0,
            .dc_low_on_data   = 0,
            .dc_low_on_param  = 0,
            .cs_high_active   = 0,
        },
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                   &io_cfg, &s_handles.io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Create ST7789 panel. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num    = LCD_RST_GPIO,
        .bits_per_pixel    = 16,
        .data_endian       = LCD_RGB_DATA_ENDIAN_BIG,
        .rgb_ele_order     = LCD_RGB_ELEMENT_ORDER_RGB,
        .flags.reset_active_high = 0,
    };
    err = esp_lcd_new_panel_st7789(s_handles.io_handle, &panel_cfg,
                                    &s_handles.panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Reset, init, turn display on. */
    esp_lcd_panel_reset(s_handles.panel_handle);
    esp_lcd_panel_init(s_handles.panel_handle);
    /* Most ST7789 IPS panels need display inversion ON for correct colors;
     * the esp_lcd ST7789 driver does not send INVON by default. */
    esp_lcd_panel_invert_color(s_handles.panel_handle, true);
    esp_lcd_panel_mirror(s_handles.panel_handle, false, false);
    esp_lcd_panel_swap_xy(s_handles.panel_handle, false);
    esp_lcd_panel_disp_on_off(s_handles.panel_handle, true);

    /* 5. Initialize LVGL port (creates LVGL task internally). */
    lvgl_port_cfg_t lvgl_cfg = {
        .task_priority  = 5,
        .task_stack     = 6 * 1024,
        .task_affinity  = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 6. Add display to LVGL. Allocate draw buffers in PSRAM. */
    uint32_t buf_size_px = LCD_H_RES * 40;  /* partial buffer, 40 rows */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = s_handles.io_handle,
        .panel_handle = s_handles.panel_handle,
        .buffer_size  = buf_size_px,
        .double_buffer = true,
        .hres         = LCD_H_RES,
        .vres         = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags        = {
            .buff_dma    = false,
            .buff_spiram = true,
            .swap_bytes  = true,  /* ST7789 expects big-endian RGB565 */
        },
    };
    s_lv_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ST7789 240x320 initialized (SCK=%d MOSI=%d CS=%d DC=%d RST=%d)",
             LCD_SCK_GPIO, LCD_MOSI_GPIO, LCD_CS_GPIO, LCD_DC_GPIO, LCD_RST_GPIO);
    return ESP_OK;
}
