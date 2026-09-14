#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;
} display_handles_t;

esp_err_t display_init(void);

#ifdef __cplusplus
}
#endif
