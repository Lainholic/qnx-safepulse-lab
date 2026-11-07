#ifndef SAFEPULSE_RINGBUFFER_H
#define SAFEPULSE_RINGBUFFER_H

#include "safepulse.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 32,768 packets * 64 bytes = exactly 2,097,152 bytes (2 MB) */
#define RINGBUFFER_CAPACITY_PACKETS 32768U
#define RINGBUFFER_MASK             (RINGBUFFER_CAPACITY_PACKETS - 1U)

typedef struct {
    safepulse_packet_t packets[RINGBUFFER_CAPACITY_PACKETS];
    volatile uint32_t head;        /* Write index (incremented by producer) */
    volatile uint32_t tail;        /* Read index (incremented by consumer) */
    volatile uint32_t drop_count;  /* Total dropped packets on buffer full */
    volatile uint32_t total_pushed;/* Total packets successfully enqueued */
} ringbuffer_t;

/* Ring buffer operations */
void ringbuffer_init(ringbuffer_t *rb);
bool ringbuffer_push(ringbuffer_t *rb, const safepulse_packet_t *pkt);
bool ringbuffer_pop(ringbuffer_t *rb, safepulse_packet_t *out_pkt);
uint32_t ringbuffer_count(const ringbuffer_t *rb);
bool ringbuffer_is_empty(const ringbuffer_t *rb);
bool ringbuffer_is_full(const ringbuffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_RINGBUFFER_H */
