#include "ringbuf.h"
#include "esp_heap_caps.h"
#include <string.h>

ringbuf_t *ringbuf_create(size_t capacity)
{
    ringbuf_t *rb = heap_caps_malloc(sizeof(ringbuf_t), MALLOC_CAP_SPIRAM);
    if (!rb) return NULL;
    rb->buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!rb->buf) {
        heap_caps_free(rb);
        return NULL;
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    return rb;
}

void ringbuf_destroy(ringbuf_t *rb)
{
    if (!rb) return;
    heap_caps_free(rb->buf);
    heap_caps_free(rb);
}

size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) return 0;
    size_t free = rb->capacity - rb->count;
    size_t to_write = len < free ? len : free;
    if (to_write == 0) return 0;

    size_t first_part = rb->capacity - rb->tail;
    if (first_part > to_write) first_part = to_write;

    memcpy(rb->buf + rb->tail, data, first_part);
    if (to_write > first_part) {
        memcpy(rb->buf, data + first_part, to_write - first_part);
    }

    rb->tail = (rb->tail + to_write) % rb->capacity;
    rb->count += to_write;
    return to_write;
}

size_t ringbuf_read(ringbuf_t *rb, uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) return 0;
    size_t to_read = len < rb->count ? len : rb->count;
    if (to_read == 0) return 0;

    size_t first_part = rb->capacity - rb->head;
    if (first_part > to_read) first_part = to_read;

    memcpy(data, rb->buf + rb->head, first_part);
    if (to_read > first_part) {
        memcpy(data + first_part, rb->buf, to_read - first_part);
    }

    rb->head = (rb->head + to_read) % rb->capacity;
    rb->count -= to_read;
    return to_read;
}

size_t ringbuf_available(ringbuf_t *rb) { return rb ? rb->count : 0; }
size_t ringbuf_free(ringbuf_t *rb) { return rb ? rb->capacity - rb->count : 0; }

void ringbuf_reset(ringbuf_t *rb)
{
    if (!rb) return;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

bool ringbuf_is_empty(ringbuf_t *rb) { return rb ? rb->count == 0 : true; }
