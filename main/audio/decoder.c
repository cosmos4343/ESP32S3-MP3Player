#include "decoder.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_types.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "DECODER";

/* Largest MP3 frame: 1152 samples x 2ch x 2B = 4608 B; 8K is plenty. */
#define FRAME_OUT_SIZE   8192
/* Player feeds <= 4096 B; keep room for leftovers carried over. */
#define HOLD_SIZE        8192
#define SCRATCH_SIZE     (4096 + HOLD_SIZE)

static esp_audio_simple_dec_handle_t simple_dec = NULL;
static decoder_info_t info = {0};
static bool done = true;
static FILE *fp = NULL;
static uint8_t *pcm_cache = NULL;
static size_t pcm_cache_size = 0;
static uint8_t *frame_out = NULL;   /* heap output buffer (never on stack) */
static uint8_t *scratch = NULL;    /* holdover + new feed, contiguous */
static uint8_t hold[HOLD_SIZE];    /* bytes the parser did not consume */
static size_t hold_len = 0;
static bool info_logged = false;
static size_t bytes_fed = 0;
static bool hdr_logged = false;
static decoder_stats_t st = {0};

/* Parse the first MP3 frame sync to learn the true stream format.
 * Works even when the decoder's get_info() is uncooperative. */
static void parse_mp3_header(const uint8_t *data, size_t len)
{
    if (hdr_logged || bytes_fed > 256 * 1024) return;
    static const uint16_t sr_tbl[4][3] = {
        {44100, 48000, 32000},   /* MPEG1 */
        {22050, 24000, 16000},   /* MPEG2 */
        {11025, 12000,  8000},   /* MPEG2.5 */
    };
    for (size_t i = 0; i + 3 < len; i++) {
        if ((data[i] & 0xFF) != 0xFF || (data[i + 1] & 0xE0) != 0xE0) continue;
        uint8_t b1 = data[i + 1], b2 = data[i + 2], b3 = data[i + 3];
        int ver_bits = (b1 >> 3) & 0x3;
        int layer_bits = (b1 >> 1) & 0x3;
        int sr_idx = (b2 >> 2) & 0x3;
        int br_idx = b2 >> 4;
        if (ver_bits == 1 || layer_bits != 1 || sr_idx == 3 || br_idx == 0 || br_idx == 15) {
            continue;
        }
        int ver_row = ver_bits == 3 ? 0 : (ver_bits == 2 ? 1 : 2);
        uint32_t rate = sr_tbl[ver_row][sr_idx];
        int ch_mode = (b3 >> 6) & 0x3;
        int ch = ch_mode == 3 ? 1 : 2;
        ESP_LOGI(TAG, "mp3 hdr @%lu: ver=%d layer3 rate=%luHz ch=%d raw=%02x%02x%02x%02x",
                 (unsigned long)(bytes_fed + i), ver_bits, (unsigned long)rate,
                 ch, data[i], data[i + 1], data[i + 2], data[i + 3]);
        /* Trust the frame header immediately so I2S follows the true rate
         * even before (or if) get_info() reports it. */
        if (rate != info.sample_rate || ch != info.channels) {
            info.sample_rate = rate;
            info.channels = ch;
            info.bits_per_sample = 16;
        }
        hdr_logged = true;
        return;
    }
}

static bool decoders_registered = false;

static void register_decoders_once(void)
{
    if (decoders_registered) {
        return;
    }
    /* MP3 (and friends) live in the full decoder registry; the simple dec
     * framework queries it for types it does not natively implement. */
    esp_audio_err_t err = esp_audio_dec_register_default();
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder register failed: %d", (int)err);
    }
    decoders_registered = true;
}

esp_err_t decoder_open(const char *path)
{
    decoder_close();
    register_decoders_once();

    fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "opening decoder for: %s", path);

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,  /* feed arbitrary chunks; built-in parser finds frames */
    };

    esp_audio_err_t err = esp_audio_simple_dec_open(&dec_cfg, &simple_dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed: %d", (int)err);
        fclose(fp);
        fp = NULL;
        return ESP_FAIL;
    }

    pcm_cache = heap_caps_malloc(PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_cache) {
        ESP_LOGE(TAG, "pcm cache alloc failed");
        esp_audio_simple_dec_close(simple_dec);
        simple_dec = NULL;
        fclose(fp);
        fp = NULL;
        return ESP_ERR_NO_MEM;
    }
    frame_out = heap_caps_malloc(FRAME_OUT_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!frame_out) {
        frame_out = heap_caps_malloc(FRAME_OUT_SIZE, MALLOC_CAP_8BIT);
    }
    if (!frame_out) {
        ESP_LOGE(TAG, "frame out alloc failed");
        heap_caps_free(pcm_cache);
        pcm_cache = NULL;
        esp_audio_simple_dec_close(simple_dec);
        simple_dec = NULL;
        fclose(fp);
        fp = NULL;
        return ESP_ERR_NO_MEM;
    }
    scratch = heap_caps_malloc(SCRATCH_SIZE, MALLOC_CAP_8BIT);
    if (!scratch) {
        ESP_LOGE(TAG, "scratch alloc failed");
        heap_caps_free(frame_out);
        frame_out = NULL;
        heap_caps_free(pcm_cache);
        pcm_cache = NULL;
        esp_audio_simple_dec_close(simple_dec);
        simple_dec = NULL;
        fclose(fp);
        fp = NULL;
        return ESP_ERR_NO_MEM;
    }
    pcm_cache_size = 0;
    hold_len = 0;
    info_logged = false;
    hdr_logged = false;
    bytes_fed = 0;
    memset(&st, 0, sizeof(st));
    done = false;

    /* Real format is only known after the first frame is decoded.
     * Use sane defaults until then. */
    info.sample_rate = AUDIO_SAMPLE_RATE;
    info.bits_per_sample = 16;
    info.channels = 2;
    info.is_pcm = true;

    return ESP_OK;
}

/* Run process() over raw until it is consumed or the parser asks for more
 * data. Decoded PCM is appended to pcm_cache. Mirrors the official
 * simple_decoder_test loop: raw.buffer/len MUST be advanced by consumed,
 * and unconsumed bytes MUST be preserved across calls (else stream corrupt). */
static void append_pcm(const uint8_t *src, size_t len)
{
    size_t avail = PCM_BUF_SIZE - pcm_cache_size;
    size_t copy = len < avail ? len : avail;
    if (copy < len) {
        ESP_LOGW(TAG, "pcm cache full, dropped %lu bytes",
                 (unsigned long)(len - copy));
        st.pcm_dropped += (uint32_t)(len - copy);
    }
    memcpy(pcm_cache + pcm_cache_size, src, copy);
    pcm_cache_size += copy;
}

static esp_audio_err_t drain(esp_audio_simple_dec_raw_t *raw, bool eos)
{
    while (raw->len > 0) {
        esp_audio_simple_dec_out_t out_frame = {
            .buffer = frame_out,
            .len = FRAME_OUT_SIZE,
        };

        esp_audio_err_t err = esp_audio_simple_dec_process(simple_dec, raw, &out_frame);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "need %d output bytes", (int)out_frame.needed_size);
            return err;
        }

        if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_DATA_LACK) {
            ESP_LOGW(TAG, "decoder process err: %d (len=%lu consumed=%lu)",
                     (int)err, (unsigned long)raw->len, (unsigned long)raw->consumed);
            st.hard_err++;
            return err;
        }

        if (out_frame.decoded_size > 0) {
            if (!info_logged) {
                esp_audio_simple_dec_info_t dec_info = {0};
                esp_audio_err_t ir = esp_audio_simple_dec_get_info(simple_dec, &dec_info);
                ESP_LOGI(TAG, "first frame: get_info ret=%d rate=%lu ch=%d bits=%d",
                         (int)ir, (unsigned long)dec_info.sample_rate,
                         dec_info.channel, dec_info.bits_per_sample);
                if (ir == ESP_AUDIO_ERR_OK && dec_info.sample_rate > 0) {
                    info.sample_rate = dec_info.sample_rate;
                    info.bits_per_sample = dec_info.bits_per_sample;
                    info.channels = dec_info.channel;
                    ESP_LOGI(TAG, "stream format: %luHz %dch %dbps bitrate=%lu",
                             (unsigned long)info.sample_rate, info.channels,
                             info.bits_per_sample, (unsigned long)dec_info.bitrate);
                    info_logged = true;
                }
            }
            append_pcm(out_frame.buffer, out_frame.decoded_size);
            st.frames_ok++;
        } else if (err == ESP_AUDIO_ERR_OK && raw->consumed > 0) {
            st.ok_nopcm++;
        }

        /* Advance past the bytes the parser actually consumed. */
        raw->len -= raw->consumed;
        raw->buffer += raw->consumed;

        if (err == ESP_AUDIO_ERR_DATA_LACK) {
            /* Parser buffered everything and needs more file bytes. */
            return ESP_AUDIO_ERR_DATA_LACK;
        }

        /* No progress guard: avoid spinning on OK with zero movement. */
        if (raw->consumed == 0 && out_frame.decoded_size == 0) {
            return ESP_AUDIO_ERR_DATA_LACK;
        }
    }

    if (eos) {
        /* Final empty eos call to flush any cached last frame. */
        static uint8_t dummy;
        esp_audio_simple_dec_raw_t eos_raw = {
            .buffer = &dummy,
            .len = 0,
            .eos = true,
        };
        esp_audio_simple_dec_out_t out_frame = {
            .buffer = frame_out,
            .len = FRAME_OUT_SIZE,
        };
        esp_audio_err_t err = esp_audio_simple_dec_process(simple_dec, &eos_raw, &out_frame);
        if (err == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0) {
            append_pcm(out_frame.buffer, out_frame.decoded_size);
        }
    }

    return ESP_AUDIO_ERR_OK;
}

esp_err_t decoder_feed(const uint8_t *in, size_t in_len)
{
    if (!simple_dec || done) return ESP_ERR_INVALID_STATE;
    if (hold_len + in_len > SCRATCH_SIZE) {
        ESP_LOGE(TAG, "feed overflow: hold=%lu in=%lu", (unsigned long)hold_len,
                 (unsigned long)in_len);
        return ESP_ERR_NO_MEM;
    }

    parse_mp3_header(in, in_len);
    bytes_fed += in_len;

    /* Stitch unconsumed leftovers in front of the new chunk so the parser
     * always sees a gapless byte stream. */
    if (hold_len) {
        memcpy(scratch, hold, hold_len);
    }
    memcpy(scratch + hold_len, in, in_len);

    esp_audio_simple_dec_raw_t raw_in = {
        .buffer = scratch,
        .len = hold_len + in_len,
        .eos = false,
    };

    drain(&raw_in, false);

    /* Preserve whatever the parser did not consume for the next feed. */
    hold_len = raw_in.len;
    if (hold_len > HOLD_SIZE) {
        ESP_LOGE(TAG, "holdover overflow %lu", (unsigned long)hold_len);
        hold_len = HOLD_SIZE;
    }
    if (hold_len) {
        memcpy(hold, raw_in.buffer, hold_len);
    }
    return ESP_OK;
}

/* Flush the decoder's internally buffered final frame(s). */
esp_err_t decoder_flush(void)
{
    if (!simple_dec || done) return ESP_ERR_INVALID_STATE;

    esp_audio_simple_dec_raw_t raw_in = {
        .buffer = hold_len ? hold : NULL,
        .len = hold_len,
        .eos = true,
    };
    drain(&raw_in, true);
    hold_len = 0;
    done = true;
    return ESP_OK;
}

esp_err_t decoder_read_pcm(uint8_t *out, size_t out_size, size_t *out_len)
{
    *out_len = 0;
    if (!simple_dec) return ESP_ERR_INVALID_STATE;

    if (info.channels == 1) {
        /* Upmix mono -> stereo in-place safe expansion (tail -> head). */
        size_t frames_avail = pcm_cache_size / 2;
        size_t frames_out = out_size / 4;
        if (frames_out > frames_avail) frames_out = frames_avail;
        const int16_t *src = (const int16_t *)pcm_cache;
        int16_t *dst = (int16_t *)out;
        for (size_t i = frames_out; i > 0; ) {
            i--;
            dst[2 * i] = src[i];
            dst[2 * i + 1] = src[i];
        }
        size_t consumed = frames_out * 2;
        size_t remain = pcm_cache_size - consumed;
        if (remain) {
            memmove(pcm_cache, pcm_cache + consumed, remain);
        }
        pcm_cache_size = remain;
        *out_len = frames_out * 4;
        return ESP_OK;
    }

    size_t copy = out_size < pcm_cache_size ? out_size : pcm_cache_size;
    copy &= ~3u;  /* keep stereo 16-bit frame alignment */
    memcpy(out, pcm_cache, copy);
    if (pcm_cache_size > copy) {
        memmove(pcm_cache, pcm_cache + copy, pcm_cache_size - copy);
    }
    pcm_cache_size -= copy;
    *out_len = copy;

    return ESP_OK;
}

const decoder_info_t *decoder_get_info(void) { return &info; }

void decoder_get_stats(decoder_stats_t *out)
{
    if (out) {
        *out = st;
        out->fed_bytes = bytes_fed;
    }
}

void decoder_close(void)
{
    if (simple_dec) {
        esp_audio_simple_dec_close(simple_dec);
        simple_dec = NULL;
    }
    if (fp) {
        fclose(fp);
        fp = NULL;
    }
    if (pcm_cache) {
        heap_caps_free(pcm_cache);
        pcm_cache = NULL;
    }
    if (frame_out) {
        heap_caps_free(frame_out);
        frame_out = NULL;
    }
    if (scratch) {
        heap_caps_free(scratch);
        scratch = NULL;
    }
    hold_len = 0;
    pcm_cache_size = 0;
    done = true;
    memset(&info, 0, sizeof(info));
}

bool decoder_is_done(void) { return done && pcm_cache_size == 0; }

FILE *decoder_get_fp(void) { return fp; }
