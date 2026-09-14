#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t file_list_scan(const char *base_path);
size_t file_list_count(void);
const char *file_list_get(size_t index);
void file_list_next(void);
void file_list_prev(void);
void file_list_set_current(size_t index);
const char *file_list_current(void);
size_t file_list_current_index(void);
void file_list_free(void);
