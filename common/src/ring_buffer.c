#include "ring_buffer.h"

static bool ring_buffer_is_initialized(const ring_buffer_t *ring)
{
    return (ring != NULL) && (ring->buffer != NULL) && (ring->capacity > 0u);
}

unified_error_t ring_buffer_init(ring_buffer_t *ring, uint8_t *storage, size_t capacity)
{
    if ((ring == NULL) || (storage == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (capacity == 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    ring->buffer = storage;
    ring->capacity = capacity;
    ring->read_index = 0u;
    ring->write_index = 0u;
    ring->size = 0u;

    return UNIFIED_OK;
}

void ring_buffer_reset(ring_buffer_t *ring)
{
    if (!ring_buffer_is_initialized(ring)) {
        return;
    }

    ring->read_index = 0u;
    ring->write_index = 0u;
    ring->size = 0u;
}

bool ring_buffer_is_empty(const ring_buffer_t *ring)
{
    return (!ring_buffer_is_initialized(ring)) || (ring->size == 0u);
}

bool ring_buffer_is_full(const ring_buffer_t *ring)
{
    return ring_buffer_is_initialized(ring) && (ring->size == ring->capacity);
}

size_t ring_buffer_available(const ring_buffer_t *ring)
{
    if (!ring_buffer_is_initialized(ring)) {
        return 0u;
    }

    return ring->size;
}

size_t ring_buffer_free_space(const ring_buffer_t *ring)
{
    if (!ring_buffer_is_initialized(ring)) {
        return 0u;
    }

    return ring->capacity - ring->size;
}

unified_error_t ring_buffer_push(ring_buffer_t *ring, uint8_t byte)
{
    if (!ring_buffer_is_initialized(ring)) {
        return UNIFIED_ERR_NULL;
    }

    if (ring_buffer_is_full(ring)) {
        return UNIFIED_ERR_RING_BUFFER_FULL;
    }

    ring->buffer[ring->write_index] = byte;
    ring->write_index = (ring->write_index + 1u) % ring->capacity;
    ring->size++;

    return UNIFIED_OK;
}

unified_error_t ring_buffer_pop(ring_buffer_t *ring, uint8_t *byte)
{
    if ((!ring_buffer_is_initialized(ring)) || (byte == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (ring_buffer_is_empty(ring)) {
        return UNIFIED_ERR_RING_BUFFER_EMPTY;
    }

    *byte = ring->buffer[ring->read_index];
    ring->read_index = (ring->read_index + 1u) % ring->capacity;
    ring->size--;

    return UNIFIED_OK;
}

size_t ring_buffer_write(ring_buffer_t *ring, const uint8_t *data, size_t length)
{
    size_t written = 0u;

    if ((!ring_buffer_is_initialized(ring)) || (data == NULL)) {
        return 0u;
    }

    while ((written < length) && !ring_buffer_is_full(ring)) {
        ring->buffer[ring->write_index] = data[written];
        ring->write_index = (ring->write_index + 1u) % ring->capacity;
        ring->size++;
        written++;
    }

    return written;
}

size_t ring_buffer_read(ring_buffer_t *ring, uint8_t *data, size_t length)
{
    size_t read_count = 0u;

    if ((!ring_buffer_is_initialized(ring)) || (data == NULL)) {
        return 0u;
    }

    while ((read_count < length) && !ring_buffer_is_empty(ring)) {
        data[read_count] = ring->buffer[ring->read_index];
        ring->read_index = (ring->read_index + 1u) % ring->capacity;
        ring->size--;
        read_count++;
    }

    return read_count;
}
