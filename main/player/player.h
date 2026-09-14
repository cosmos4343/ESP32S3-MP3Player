#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
} player_state_t;

typedef enum {
    PLAYER_CMD_PLAY,
    PLAYER_CMD_PAUSE,
    PLAYER_CMD_TOGGLE,
    PLAYER_CMD_NEXT,
    PLAYER_CMD_PREV,
    PLAYER_CMD_STOP,
    PLAYER_CMD_VOL_UP,
    PLAYER_CMD_VOL_DOWN,
} player_cmd_t;

esp_err_t player_init(void);
void player_start_tasks(void);
player_state_t player_get_state(void);
int player_get_volume(void);
void player_send_cmd(player_cmd_t cmd);
const char *player_get_current_file(void);
int player_get_played_sec(void);
int player_get_total_sec(void);
esp_err_t player_play_index(size_t idx);
