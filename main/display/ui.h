#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

void ui_update(void);

/* Full-screen song list (toggled by long-press Prev/Next). */
void ui_list_enter(void);
void ui_list_exit(void);
bool ui_list_is_visible(void);
void ui_list_move_up(void);
void ui_list_move_down(void);
size_t ui_list_get_selected(void);

#ifdef __cplusplus
}
#endif
