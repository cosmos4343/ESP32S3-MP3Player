#include "power.h"
#include "esp_log.h"

static const char *TAG = "POWER";

esp_err_t power_init(void)
{
    ESP_LOGI(TAG, "power init (3.3V, no low-power features)");
    return ESP_OK;
}
