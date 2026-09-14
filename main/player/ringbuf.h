#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   head;
    size_t   tail;
    size_t   count;
} ringbuf_t;

ringbuf_t *ringbuf_create(size_t capacity);
void ringbuf_destroy(ringbuf_t *rb);
size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len);
size_t ringbuf_read(ringbuf_t *rb, uint8_t *data, size_t len);
size_t ringbuf_available(ringbuf_t *rb);
size_t ringbuf_free(ringbuf_t *rb);
void ringbuf_reset(ringbuf_t *rb);
bool ringbuf_is_empty(ringbuf_t *rb);
