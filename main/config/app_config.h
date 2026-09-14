#pragma once

// ─── Audio ───
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          2
#define AUDIO_FRAME_BYTES       (AUDIO_BITS_PER_SAMPLE / 8 * AUDIO_CHANNELS)

// ─── I2S DMA ───
/* 16 descriptors x 512 stereo frames = 32 KB (~186 ms at 44.1 kHz):
 * large enough to ride out 60-80 ms MP3 decode bursts without underrun. */
#define I2S_DMA_DESC_NUM        16
#define I2S_DMA_FRAME_NUM       240

// ─── Buffers (allocated in PSRAM) ───
#define FILE_BUF_SIZE           (256 * 1024)
#define PCM_BUF_SIZE            (128 * 1024)

// ─── File System ───
#define SD_MOUNT_POINT          "/sdcard"
#define MUSIC_DIR               "/sdcard/music"
#define MAX_FILE_PATH_LEN       256
#define MAX_PLAYLIST_SIZE       2048

// ─── Buttons ───
#define BTN_SCAN_PERIOD_MS      10
#define BTN_DEBOUNCE_MS         30
#define BTN_LONG_PRESS_MS       500
#define BTN_VOL_REPEAT_MS       200

// ─── Tasks ───
#define PLAYER_TASK_STACK       12288
#define FILE_TASK_STACK         4096
#define BUTTON_TASK_STACK       4096
#define MONITOR_TASK_STACK      2048

#define PLAYER_TASK_PRIO        8
#define FILE_TASK_PRIO          5
#define BUTTON_TASK_PRIO        3
#define MONITOR_TASK_PRIO       1
