#ifndef SAFEPULSE_RINGBUFFER_H
#define SAFEPULSE_RINGBUFFER_H

#include "safepulse.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 32,768 packets * 64 bytes = exactly 2,097,152 bytes (2 MB) */
#define RINGBUFFER_CAPACITY_PACKETS 32768U
#define RINGBUFFER_MASK             (RINGBUFFER_CAPACITY_PACKETS - 1U)

typedef struct {
    _Atomic uint32_t head;        /* Atomic head index with release semantics */
    _Atomic uint32_t tail;        /* Atomic tail index with release semantics */
    _Atomic uint32_t drop_count;  /* Atomic drop counter on buffer full */
    _Atomic uint32_t total_pushed;/* Atomic monotonic counter of total packets */
    safepulse_packet_t packets[RINGBUFFER_CAPACITY_PACKETS];
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
