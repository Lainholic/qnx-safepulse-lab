#include "ringbuffer.h"
#include <string.h>
#include <stdatomic.h>

/* IEEE 802.3 CRC-32 polynomial 0xEDB88320 */
static const uint32_t crc32_tab[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
};

uint32_t safepulse_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc32_tab[crc & 0x0F];
        crc = (crc >> 4) ^ crc32_tab[crc & 0x0F];
    }
    return ~crc;
}

void ringbuffer_init(ringbuffer_t *rb) {
    if (!rb) return;
    memset(rb, 0, sizeof(ringbuffer_t));
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    atomic_init(&rb->drop_count, 0);
    atomic_init(&rb->total_pushed, 0);
    atomic_init(&rb->persistent_seq, 0);
}

uint32_t ringbuffer_count(const ringbuffer_t *rb) {
    if (!rb) return 0;
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (head - tail);
}

bool ringbuffer_is_empty(const ringbuffer_t *rb) {
    if (!rb) return true;
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (head == tail);
}

bool ringbuffer_is_full(const ringbuffer_t *rb) {
    if (!rb) return true;
    return (ringbuffer_count(rb) >= RINGBUFFER_CAPACITY_PACKETS);
}

bool ringbuffer_push(ringbuffer_t *rb, const safepulse_packet_t *pkt) {
    if (!rb || !pkt) return false;

    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if ((head - tail) >= RINGBUFFER_CAPACITY_PACKETS) {
        atomic_fetch_add_explicit(&rb->drop_count, 1, memory_order_relaxed);
        return false;
    }

    uint32_t index = head & RINGBUFFER_MASK;

    /* Write packet data with release semantics - ensures payload visible before head update */
    memcpy(&rb->packets[index], pkt, sizeof(safepulse_packet_t));
    atomic_thread_fence(memory_order_release);

    /* Update head with release - makes data+head visible atomically to consumer */
    atomic_store_explicit(&rb->head, head + 1, memory_order_release);
    atomic_fetch_add_explicit(&rb->total_pushed, 1, memory_order_relaxed);

    return true;
}

bool ringbuffer_pop(ringbuffer_t *rb, safepulse_packet_t *out_pkt) {
    if (!rb || !out_pkt) return false;

    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (head == tail) {
        return false; /* Empty */
    }

    uint32_t index = tail & RINGBUFFER_MASK;

    /* Acquire barrier ensures we see the data written by producer */
    atomic_thread_fence(memory_order_acquire);
    memcpy(out_pkt, &rb->packets[index], sizeof(safepulse_packet_t));

    /* Update tail with release - prevents producer from overwriting */
    atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);

    return true;
}
