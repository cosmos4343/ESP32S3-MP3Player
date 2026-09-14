#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t button_init(void);
void button_start_task(void);
