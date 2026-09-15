#include "button.h"
#include "board.h"
#include "app_config.h"
#include "player.h"
#include "file_list.h"
#include "ui.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

typedef struct {
    gpio_num_t gpio;
    player_cmd_t short_cmd;
    bool pressed;
    uint32_t press_start_ms;
    bool long_press_sent;
    uint32_t last_repeat_ms;
} btn_state_t;

static btn_state_t buttons[BTN_COUNT];

static const struct {
    gpio_num_t gpio;
    player_cmd_t cmd;
} btn_cfg[] = {
    {BTN_PLAY_GPIO, PLAYER_CMD_TOGGLE},
    {BTN_PREV_GPIO, PLAYER_CMD_PREV},
    {BTN_NEXT_GPIO, PLAYER_CMD_NEXT},
    {BTN_VOLP_GPIO, PLAYER_CMD_VOL_UP},
    {BTN_VOLM_GPIO, PLAYER_CMD_VOL_DOWN},
};

static bool is_volume_btn(int idx)
{
    return buttons[idx].short_cmd == PLAYER_CMD_VOL_UP ||
           buttons[idx].short_cmd == PLAYER_CMD_VOL_DOWN;
}

static bool is_prev_next_btn(int idx)
{
    return buttons[idx].short_cmd == PLAYER_CMD_PREV ||
           buttons[idx].short_cmd == PLAYER_CMD_NEXT;
}

/* Long-press action for Prev/Next: toggle the full-screen song list. */
static void handle_prev_next_longpress(int idx)
{
    ESP_LOGI(TAG, "long press btn %d, list_visible=%d", idx, ui_list_is_visible());
    if (ui_list_is_visible()) {
        ui_list_exit();
    } else {
        ui_list_enter();
        ESP_LOGI(TAG, "after enter: list_visible=%d", ui_list_is_visible());
    }
}

/* Short-press action while the song list is visible. */
static void handle_short_press_in_list(int idx)
{
    player_cmd_t c = buttons[idx].short_cmd;
    if (c == PLAYER_CMD_PREV) {
        ui_list_move_up();
    } else if (c == PLAYER_CMD_NEXT) {
        ui_list_move_down();
    } else if (c == PLAYER_CMD_TOGGLE) {
        /* Play selected song and exit list.
         * Send STOP first so playback restarts with the new file
         * even if currently playing/paused. */
        file_list_set_current(ui_list_get_selected());
        ui_list_exit();
        player_send_cmd(PLAYER_CMD_STOP);
        player_send_cmd(PLAYER_CMD_PLAY);
    }
}

static void button_task(void *arg)
{
    ESP_LOGI(TAG, "button_task started on Core %d", xPortGetCoreID());

    uint32_t now = 0;

    while (1) {
        now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        for (int i = 0; i < BTN_COUNT; i++) {
            int level = gpio_get_level(buttons[i].gpio);
            bool cur_pressed = (level == 0);

            if (cur_pressed && !buttons[i].pressed) {
                /* ── Button just pressed ── */
                buttons[i].pressed = true;
                buttons[i].press_start_ms = now;
                buttons[i].long_press_sent = false;
                buttons[i].last_repeat_ms = now;
            } else if (!cur_pressed && buttons[i].pressed) {
                /* ── Button just released ── */
                uint32_t held = now - buttons[i].press_start_ms;
                if (held >= BTN_DEBOUNCE_MS && !buttons[i].long_press_sent) {
                    /* Short press action */
                    if (ui_list_is_visible()) {
                        handle_short_press_in_list(i);
                    } else {
                        /* Normal playback mode */
                        player_cmd_t c = buttons[i].short_cmd;
                        if (c == PLAYER_CMD_TOGGLE) {
                            if (player_get_state() == PLAYER_STATE_STOPPED) {
                                file_list_set_current(ui_list_get_selected());
                            }
                            player_send_cmd(c);
                        } else {
                            player_send_cmd(c);
                        }
                    }
                }
                buttons[i].pressed = false;
            } else if (cur_pressed && buttons[i].pressed) {
                /* ── Button held ── */
                uint32_t held = now - buttons[i].press_start_ms;
                if (held >= BTN_LONG_PRESS_MS && !buttons[i].long_press_sent) {
                    buttons[i].long_press_sent = true;
                    /* Long-press action */
                    if (is_prev_next_btn(i)) {
                        handle_prev_next_longpress(i);
                    }
                    /* Volume keys start auto-repeat after long-press threshold */
                }
                /* Volume keys auto-repeat while held. */
                if (is_volume_btn(i) && buttons[i].long_press_sent) {
                    uint32_t since_repeat = now - buttons[i].last_repeat_ms;
                    if (since_repeat >= BTN_VOL_REPEAT_MS) {
                        player_send_cmd(buttons[i].short_cmd);
                        buttons[i].last_repeat_ms = now;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_SCAN_PERIOD_MS));
    }
}

esp_err_t button_init(void)
{
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    for (int i = 0; i < BTN_COUNT; i++) {
        buttons[i].gpio = btn_cfg[i].gpio;
        buttons[i].short_cmd = btn_cfg[i].cmd;
        buttons[i].pressed = false;
        buttons[i].press_start_ms = 0;
        buttons[i].long_press_sent = false;
        buttons[i].last_repeat_ms = 0;

        io_conf.pin_bit_mask = (1ULL << btn_cfg[i].gpio);
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }
    return ESP_OK;
}

void button_start_task(void)
{
    xTaskCreatePinnedToCore(button_task, "button_task", BUTTON_TASK_STACK,
                            NULL, BUTTON_TASK_PRIO, NULL, 1);
}
