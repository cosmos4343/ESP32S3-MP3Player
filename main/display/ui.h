#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

void ui_update(void);

/* File list browsing (used in STOPPED state). */
void ui_list_show(void);
void ui_list_hide(void);
void ui_list_move_up(void);
void ui_list_move_down(void);
size_t ui_list_get_selected(void);

#ifdef __cplusplus
}
#endif
