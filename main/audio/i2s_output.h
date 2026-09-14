#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

esp_err_t i2s_output_init(void);
esp_err_t i2s_output_write(const uint8_t *data, size_t len, size_t *bytes_written, TickType_t timeout);
void i2s_output_set_sample_rate(uint32_t rate);
void i2s_output_deinit(void);
