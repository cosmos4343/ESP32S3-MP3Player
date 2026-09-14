#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/stat.h>
#include <sys/types.h>

#include "board.h"
#include "sd_card.h"
#include "file_list.h"
#include "i2s_output.h"
#include "player.h"
#include "button.h"
#include "power.h"
#include "app_config.h"
#include "display.h"
#include "ui.h"

static const char *TAG = "MAIN";

static void monitor_task(void *arg)
{
    bool list_shown = false;
    while (1) {
        player_state_t state = player_get_state();

        ui_update();

        /* Toggle file list visibility based on state transitions. */
        if (state == PLAYER_STATE_STOPPED && !list_shown) {
            ui_list_show();
            list_shown = true;
        } else if (state != PLAYER_STATE_STOPPED && list_shown) {
            ui_list_hide();
            list_shown = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, "  MP3 Player booting...");
    ESP_LOGI(TAG, "==============================");

    esp_reset_reason_t rr = esp_reset_reason();
    if (rr != ESP_RST_POWERON) {
        ESP_LOGW(TAG, "last reset reason: %d (2=SW 3=PANIC 4=INT_WDT 5=TASK_WDT 9=BROWNOUT)", rr);
    }

    ESP_ERROR_CHECK(power_init());

    /* Bring up the display first so it works even without an SD card. */
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());

    /* SD card is optional: a missing/unresponsive card must not abort boot. */
    esp_err_t sd_err = sd_card_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unavailable (%s); continuing without it",
                 esp_err_to_name(sd_err));
    } else {
        /* Scan the whole card recursively so MP3s in root or /music both work. */
        esp_err_t list_err = file_list_scan(SD_MOUNT_POINT);
        if (list_err != ESP_OK) {
            ESP_LOGW(TAG, "no files found, try creating %s", MUSIC_DIR);
            mkdir(MUSIC_DIR, 0755);
        } else {
            ESP_LOGI(TAG, "found %zu music files", file_list_count());
            for (size_t i = 0; i < file_list_count() && i < 5; i++) {
                ESP_LOGI(TAG, "  [%zu] %s", i, file_list_get(i));
            }
        }
        /* Show file list on startup (state is STOPPED). */
        ui_list_show();
    }

    ESP_ERROR_CHECK(i2s_output_init());

    ESP_ERROR_CHECK(player_init());

    ESP_ERROR_CHECK(button_init());

    player_start_tasks();
    button_start_task();

    xTaskCreatePinnedToCore(monitor_task, "monitor_task", MONITOR_TASK_STACK,
                            NULL, MONITOR_TASK_PRIO, NULL, 1);

    ESP_LOGI(TAG, "All systems ready. Press Play to start.");
}
