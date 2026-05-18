#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通用字节环形缓冲区。
 *
 * 不使用动态内存，调用方负责提供 storage。
 * 本模块不内置锁；若在 ISR/任务或多线程之间共享，调用方需要自行加锁或关中断保护。
 */
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t size;
} ring_buffer_t;

unified_error_t ring_buffer_init(ring_buffer_t *ring, uint8_t *storage, size_t capacity);
void ring_buffer_reset(ring_buffer_t *ring);

bool ring_buffer_is_empty(const ring_buffer_t *ring);
bool ring_buffer_is_full(const ring_buffer_t *ring);
size_t ring_buffer_available(const ring_buffer_t *ring);
size_t ring_buffer_free_space(const ring_buffer_t *ring);

unified_error_t ring_buffer_push(ring_buffer_t *ring, uint8_t byte);
unified_error_t ring_buffer_pop(ring_buffer_t *ring, uint8_t *byte);

size_t ring_buffer_write(ring_buffer_t *ring, const uint8_t *data, size_t length);
size_t ring_buffer_read(ring_buffer_t *ring, uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
