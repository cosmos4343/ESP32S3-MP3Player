#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
    bool     is_pcm;
} decoder_info_t;

typedef struct {
    uint32_t frames_ok;   /* process() calls that produced PCM */
    uint32_t pcm_dropped; /* PCM bytes discarded because cache was full */
    uint32_t hard_err;    /* process() errors other than DATA_LACK */
    uint32_t ok_nopcm;    /* OK, bytes consumed, but zero PCM produced */
    size_t   fed_bytes;   /* compressed file bytes fed (counted once) */
} decoder_stats_t;

void decoder_get_stats(decoder_stats_t *out);

esp_err_t decoder_open(const char *path);
esp_err_t decoder_feed(const uint8_t *in, size_t in_len);
esp_err_t decoder_flush(void);
esp_err_t decoder_read_pcm(uint8_t *out, size_t out_size, size_t *out_len);
const decoder_info_t *decoder_get_info(void);
FILE *decoder_get_fp(void);
void decoder_close(void);
bool decoder_is_done(void);
