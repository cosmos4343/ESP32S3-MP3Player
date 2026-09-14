#include "player.h"
#include "ringbuf.h"
#include "app_config.h"
#include "file_list.h"
#include "i2s_output.h"
#include "decoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PLAYER";

static player_state_t state = PLAYER_STATE_STOPPED;
static QueueHandle_t cmd_queue = NULL;
static int volume = 30;
static bool file_eof = false;
static int read_err_retries = 0;
static char s_current_file[256] = {0};
static int s_played_sec = 0;
static uint32_t s_played_ms = 0;  /* sub-second accumulator for progress */
static int s_total_sec = -1;  /* -1 = unknown (probe failed or not playing) */
static uint32_t s_i2s_rate = 0;  /* 0 = follow the first decoded frame */

#define I2S_WRITE_CHUNK  4096

static ringbuf_t *file_rb = NULL;
FILE *current_fp = NULL;
/* Set by file_task only around fread(); player_task waits for it to clear
 * before decoder_close()/fclose() to avoid a cross-core use-after-free when
 * rapidly switching tracks. */
static volatile bool s_file_reading = false;

/* Block until file_task is guaranteed not to touch current_fp.
 * Caller must first move `state` away from PLAYING. */
static void wait_file_reader_idle(void)
{
    /* Let file_task pass the (few-instruction) window between its state
     * check and claiming the flag before we test it the first time. */
    vTaskDelay(pdMS_TO_TICKS(5));
    int waited_ms = 5;
    while (s_file_reading && waited_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(2));
        waited_ms += 2;
    }
}

static void apply_volume(int16_t *samples, size_t count)
{
    int32_t scale = volume * 328;
    for (size_t i = 0; i < count; i++) {
        int32_t v = (int32_t)samples[i] * scale / 32768;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
}

/* ---- MP3 duration probe ---- */

static const uint16_t mp3_br_l3[2][15] = {
    /* MPEG1 Layer III */
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    /* MPEG2 / 2.5 Layer III */
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
};
static const uint16_t mp3_sr[3][4] = {
    {44100, 48000, 32000, 0},  /* MPEG1 */
    {22050, 24000, 16000, 0},  /* MPEG2 */
    {11025, 12000,  8000, 0},  /* MPEG2.5 */
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Estimate total seconds by parsing the first frame header plus the
 * Xing/Info/VBRI VBR header when present; otherwise fall back to a CBR
 * estimate from file size and first-frame bitrate. -1 = unknown. */
static int mp3_probe_total_sec(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);

    /* Skip an ID3v2 tag at the start of the file. */
    long data_start = 0;
    fseek(fp, 0, SEEK_SET);
    uint8_t id3[10];
    if (fread(id3, 1, 10, fp) == 10 && memcmp(id3, "ID3", 3) == 0) {
        uint32_t sz = ((id3[6] & 0x7F) << 21) | ((id3[7] & 0x7F) << 14) |
                      ((id3[8] & 0x7F) << 7) | (id3[9] & 0x7F);
        data_start = 10 + sz + ((id3[5] & 0x10) ? 10 : 0);  /* +footer */
    }

    /* Trailing ID3v1 tag would inflate a CBR estimate. */
    long tail = 0;
    if (fsize > 128) {
        uint8_t tag[3];
        fseek(fp, fsize - 128, SEEK_SET);
        if (fread(tag, 1, 3, fp) == 3 && memcmp(tag, "TAG", 3) == 0) {
            tail = 128;
        }
    }

    fseek(fp, data_start, SEEK_SET);
    uint8_t buf[2048];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 64) return -1;

    /* Find the first valid Layer III frame header. */
    int ver = 0, mono = 0;
    uint32_t kbps = 0, srate = 0, spf = 0;
    long frame_off = -1;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int veridx = (buf[i + 1] >> 3) & 3;   /* 0=2.5 1=res 2=2 3=1 */
        int layer  = (buf[i + 1] >> 1) & 3;   /* 1 = Layer III */
        int bri    = (buf[i + 2] >> 4) & 0xF;
        int sri    = (buf[i + 2] >> 2) & 3;
        int pad    = (buf[i + 2] >> 1) & 1;
        if (veridx == 1 || layer != 1 || bri == 0 || bri >= 15 || sri == 3) continue;
        uint32_t k = mp3_br_l3[veridx == 3 ? 0 : 1][bri];
        uint32_t sr = mp3_sr[veridx == 3 ? 0 : (veridx == 2 ? 1 : 2)][sri];
        uint32_t f = (uint32_t)((uint64_t)((veridx == 3) ? 1152 : 576) / 8 * k * 1000 / sr) + pad;
        if (f < 24 || f > 2048) continue;
        /* Reject false syncs: the next frame must also sync when in range. */
        if (i + f + 4 <= n &&
            !(buf[i + f] == 0xFF && (buf[i + f + 1] & 0xE0) == 0xE0)) {
            continue;
        }
        ver = (veridx == 3) ? 1 : 2;
        kbps = k; srate = sr; spf = (ver == 1) ? 1152 : 576;
        mono = ((buf[i + 2] >> 6) & 3) == 3;
        frame_off = (long)i;
        break;
    }
    if (frame_off < 0 || kbps == 0 || srate == 0) return -1;

    /* Xing/Info (VBR) or VBRI frame count beats any bitrate estimate. */
    long frames = -1;
    int side = (ver == 1) ? (mono ? 17 : 32) : (mono ? 9 : 17);
    long xoff = frame_off + 4 + side + ((buf[frame_off + 1] & 1) ? 2 : 0);
    long voff = frame_off + 36;
    if (xoff + 8 <= (long)n &&
        (memcmp(&buf[xoff], "Xing", 4) == 0 || memcmp(&buf[xoff], "Info", 4) == 0)) {
        if ((be32(&buf[xoff + 4]) & 1) && xoff + 12 <= (long)n) {
            frames = (long)be32(&buf[xoff + 8]);
        }
    } else if (voff + 18 <= (long)n && memcmp(&buf[voff], "VBRI", 4) == 0) {
        frames = (long)be32(&buf[voff + 14]);
    }

    long dur_ms;
    if (frames > 0) {
        dur_ms = (long)((uint64_t)frames * spf * 1000 / srate);
    } else {
        long audio = fsize - (data_start + frame_off) - tail;
        if (audio <= 0) return -1;
        /* bits = audio*8, ms = bits*1000/(kbps*1000) = bits/kbps */
        dur_ms = (long)((uint64_t)audio * 8 / kbps);
    }
    if (dur_ms <= 0) return -1;
    return (int)((dur_ms + 500) / 1000);
}

static void play_file(const char *path)
{
    /* Stop file_task from touching current_fp BEFORE we close it. During a
     * NEXT/PREV while playing this is a live stream; closing the FILE while
     * the other core is in fread() crashes (rapid track-switch race). */
    state = PLAYER_STATE_PAUSED;
    wait_file_reader_idle();

    decoder_close();
    ringbuf_reset(file_rb);
    file_eof = false;
    read_err_retries = 0;
    current_fp = NULL;
    s_played_sec = 0;
    s_played_ms = 0;

    /* Save basename for UI display. */
    const char *slash = strrchr(path, '/');
    snprintf(s_current_file, sizeof(s_current_file), "%s", slash ? slash + 1 : path);

    esp_err_t oerr = decoder_open(path);
    if (oerr != ESP_OK) {
        ESP_LOGE(TAG, "decoder open failed for: %s (%s)", path, esp_err_to_name(oerr));
        state = PLAYER_STATE_STOPPED;
        return;
    }

    current_fp = decoder_get_fp();

    /* Total duration for the UI progress bar / time label. */
    s_total_sec = mp3_probe_total_sec(path);
    if (s_total_sec > 0) {
        ESP_LOGI(TAG, "duration: %d:%02d", s_total_sec / 60, s_total_sec % 60);
    }

    /* Real sample rate is only known once the first frame has been decoded;
     * reconfigure I2S then (see format sync in player_task). */
    s_i2s_rate = 0;

    ESP_LOGI(TAG, "> Playing: %s", path);
    state = PLAYER_STATE_PLAYING;
}

static void file_task(void *arg)
{
    ESP_LOGI(TAG, "file_task started on Core %d", xPortGetCoreID());
    uint8_t chunk[512];  /* single-sector reads: this card garbles multi-block CMD18 bursts */

    while (1) {
        if (state != PLAYER_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Claim immediately after the state check; holds until every
         * current_fp operation this iteration is done. player_task waits on
         * this flag before fclose()-ing the file on a track switch. */
        s_file_reading = true;

        if (!current_fp || file_eof) {
            s_file_reading = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ringbuf_free(file_rb) < sizeof(chunk)) {
            s_file_reading = false;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t n = fread(chunk, 1, sizeof(chunk), current_fp);
        if (n > 0) {
            ringbuf_write(file_rb, chunk, n);
        } else if (ferror(current_fp)) {
            /* Transient SD read error (e.g. data CRC failure): retry after
             * backtracking to the sector boundary we failed on. */
            clearerr(current_fp);
            long pos = ftell(current_fp);
            fseek(current_fp, pos, SEEK_SET);
            read_err_retries++;
            ESP_LOGW(TAG, "read error, retry %d at %ld", read_err_retries, pos);
            if (read_err_retries > 20) {
                ESP_LOGE(TAG, "too many read errors, giving up track");
                file_eof = true;
            }
            s_file_reading = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        } else {
            file_eof = true;
            ESP_LOGI(TAG, "file EOF reached");
        }
        s_file_reading = false;
    }
}

static void player_task(void *arg)
{
    ESP_LOGI(TAG, "player_task started on Core %d", xPortGetCoreID());
    /* Feed <= ~2 MP3 frames per loop so a decode burst stays short
     * (~10-15 ms) and can never drain the 87 ms I2S DMA cushion. */
    uint8_t decode_in[768];
    size_t pcm_buf_size = PCM_BUF_SIZE;
    uint8_t *pcm_decode_out = heap_caps_malloc(pcm_buf_size, MALLOC_CAP_SPIRAM);
    if (!pcm_decode_out) {
        ESP_LOGE(TAG, "pcm_decode_out alloc failed");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_tick = xTaskGetTickCount();
    while (1) {
        /* Accumulate played time in ms (a single iteration is ~10 ms, so a
         * direct integer ms/1000 would almost always round down to 0). */
        TickType_t now = xTaskGetTickCount();
        uint32_t dt_ms = (uint32_t)(now - last_tick) * portTICK_PERIOD_MS;
        last_tick = now;
        if (state == PLAYER_STATE_PLAYING) {
            s_played_ms += dt_ms;
            s_played_sec = (int)(s_played_ms / 1000);
        }

        player_cmd_t cmd;
        if (xQueueReceive(cmd_queue, &cmd, 0) == pdPASS) {
            switch (cmd) {
                case PLAYER_CMD_PLAY:
                    if (state == PLAYER_STATE_STOPPED) {
                        if (file_list_count() > 0) {
                            play_file(file_list_current());
                        }
                    } else if (state == PLAYER_STATE_PAUSED) {
                        state = PLAYER_STATE_PLAYING;
                        ESP_LOGI(TAG, "▶ Resume");
                    }
                    break;

                case PLAYER_CMD_PAUSE:
                    if (state == PLAYER_STATE_PLAYING) {
                        state = PLAYER_STATE_PAUSED;
                        ESP_LOGI(TAG, "⏸ Paused");
                    }
                    break;

                case PLAYER_CMD_TOGGLE:
                    if (state == PLAYER_STATE_PLAYING) {
                        state = PLAYER_STATE_PAUSED;
                        ESP_LOGI(TAG, "⏸ Paused");
                    } else if (state == PLAYER_STATE_PAUSED || state == PLAYER_STATE_STOPPED) {
                        if (state == PLAYER_STATE_STOPPED) {
                            if (file_list_count() > 0) {
                                play_file(file_list_current());
                            }
                        } else {
                            state = PLAYER_STATE_PLAYING;
                            ESP_LOGI(TAG, "▶ Resume");
                        }
                    }
                    break;

                case PLAYER_CMD_NEXT:
                    file_list_next();
                    if (file_list_count() > 0) play_file(file_list_current());
                    break;

                case PLAYER_CMD_PREV:
                    file_list_prev();
                    if (file_list_count() > 0) play_file(file_list_current());
                    break;

                case PLAYER_CMD_STOP:
                    state = PLAYER_STATE_STOPPED;
                    wait_file_reader_idle();
                    decoder_close();
                    current_fp = NULL;
                    ringbuf_reset(file_rb);
                    file_eof = false;
                    s_played_sec = 0;
                    s_played_ms = 0;
                    s_total_sec = -1;
                    s_current_file[0] = '\0';
                    ESP_LOGI(TAG, "[] Stopped");
                    break;

                case PLAYER_CMD_VOL_UP:
                    if (volume < 100) volume += 5;
                    ESP_LOGI(TAG, "Volume: %d", volume);
                    break;

                case PLAYER_CMD_VOL_DOWN:
                    if (volume > 0) volume -= 5;
                    ESP_LOGI(TAG, "Volume: %d", volume);
                    break;
            }
        }

        if (state != PLAYER_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t available = ringbuf_available(file_rb);
        bool stream_empty = file_eof && ringbuf_is_empty(file_rb);

        if (available > 0) {
            size_t to_read = available < sizeof(decode_in) ? available : sizeof(decode_in);
            size_t n = ringbuf_read(file_rb, decode_in, to_read);
            if (n > 0) {
                esp_err_t dec_err = decoder_feed(decode_in, n);
                if (dec_err != ESP_OK) {
                    ESP_LOGW(TAG, "decoder_feed err: %s", esp_err_to_name(dec_err));
                }
            }
        } else if (stream_empty) {
            /* No more file bytes: flush the decoder's cached final frame. */
            decoder_flush();
        }

        /* Follow sample-rate changes discovered from decoded frames. */
        const decoder_info_t *di = decoder_get_info();
        if (di->sample_rate > 0 && di->sample_rate != s_i2s_rate) {
            i2s_output_set_sample_rate(di->sample_rate);
            s_i2s_rate = di->sample_rate;
        }

        size_t pcm_out = 0;
        decoder_read_pcm(pcm_decode_out, pcm_buf_size, &pcm_out);

        if (pcm_out > 0) {
            if (volume < 100) {
                apply_volume((int16_t *)pcm_decode_out, pcm_out / 2);
            }
            /* Write in chunks and honor bytes_written: a timed-out partial
             * write must not silently drop the tail of the PCM block. */
            size_t off = 0;
            while (off < pcm_out) {
                size_t chunk = pcm_out - off;
                if (chunk > I2S_WRITE_CHUNK) chunk = I2S_WRITE_CHUNK;
                size_t written = 0;
                esp_err_t werr = i2s_output_write(pcm_decode_out + off, chunk,
                                                  &written, pdMS_TO_TICKS(200));
                if (werr != ESP_OK || written == 0) {
                    if (written > 0) {
                        /* Timed out after queuing part of the chunk: keep the
                         * accepted bytes and retry the remainder. */
                        off += written;
                        continue;
                    }
                    ESP_LOGW(TAG, "i2s write err: %s (%d/%d)",
                             esp_err_to_name(werr), (int)written, (int)chunk);
                    break;
                }
                off += written;
            }
            /* Playback-rate heartbeat: PCM bytes accepted by I2S translate to
             * a known audio duration; compare it against wall clock.
             * audio_s << wall_s => DMA underruns (gaps make playback drag). */
            static size_t s_pcm_total = 0;
            static TickType_t s_stat_tick = 0;
            static decoder_stats_t s_prev_st = {0};
            s_pcm_total += off;
            TickType_t tnow = xTaskGetTickCount();
            if (s_stat_tick == 0) {
                s_stat_tick = tnow;
                decoder_get_stats(&s_prev_st);
            }
            if (tnow - s_stat_tick >= pdMS_TO_TICKS(60000)) {
                /* read_pcm always yields stereo 16-bit (mono is upmixed) */
                uint32_t rate = s_i2s_rate ? s_i2s_rate : AUDIO_SAMPLE_RATE;
                float audio_s = (float)s_pcm_total / (rate * 4u);
                float wall_s = (float)(tnow - s_stat_tick) * portTICK_PERIOD_MS / 1000.0f;
                decoder_stats_t cst;
                decoder_get_stats(&cst);
                uint32_t d_frames = cst.frames_ok - s_prev_st.frames_ok;
                ESP_LOGI(TAG, "tempo: audio=%.2fs wall=%.2fs (%luHz) frames=%lu harderr=%lu pcmdrop=%lu %s",
                         audio_s, wall_s, (unsigned long)rate,
                         (unsigned long)d_frames,
                         (unsigned long)(cst.hard_err - s_prev_st.hard_err),
                         (unsigned long)(cst.pcm_dropped - s_prev_st.pcm_dropped),
                         audio_s < wall_s - 0.25f ? "UNDERRUN!" : "ok");
                s_prev_st = cst;
                s_stat_tick = tnow;
                s_pcm_total = 0;
            }
        }

        if (stream_empty && decoder_is_done()) {
            ESP_LOGI(TAG, "song finished, auto next");
            file_list_next();
            if (file_list_count() > 0) {
                play_file(file_list_current());
            } else {
                state = PLAYER_STATE_STOPPED;
            }
            continue;
        }

        if (available == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

esp_err_t player_init(void)
{
    cmd_queue = xQueueCreate(10, sizeof(player_cmd_t));
    if (!cmd_queue) {
        ESP_LOGE(TAG, "cmd queue create failed");
        return ESP_ERR_NO_MEM;
    }

    file_rb = ringbuf_create(FILE_BUF_SIZE);
    if (!file_rb) {
        ESP_LOGE(TAG, "file ringbuf create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void player_start_tasks(void)
{
    xTaskCreatePinnedToCore(player_task, "player_task", PLAYER_TASK_STACK,
                            NULL, PLAYER_TASK_PRIO, NULL, 0);
    xTaskCreatePinnedToCore(file_task, "file_task", FILE_TASK_STACK,
                            NULL, FILE_TASK_PRIO, NULL, 1);
}

player_state_t player_get_state(void) { return state; }
int player_get_volume(void) { return volume; }
const char *player_get_current_file(void) { return s_current_file; }
int player_get_played_sec(void) { return s_played_sec; }
int player_get_total_sec(void) { return s_total_sec; }

void player_send_cmd(player_cmd_t cmd)
{
    if (cmd_queue) {
        xQueueSend(cmd_queue, &cmd, 0);
    }
}

esp_err_t player_play_index(size_t idx)
{
    const char *path = file_list_get(idx);
    if (!path) return ESP_ERR_INVALID_ARG;
    file_list_set_current(idx);
    play_file(path);
    return ESP_OK;
}
