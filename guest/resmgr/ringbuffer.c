#include "ringbuffer.h"
#include <string.h>

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
    memset((void*)rb, 0, sizeof(ringbuffer_t));
    rb->head = 0;
    rb->tail = 0;
    rb->drop_count = 0;
    rb->total_pushed = 0;
}

uint32_t ringbuffer_count(const ringbuffer_t *rb) {
    if (!rb) return 0;
    return (rb->head - rb->tail);
}

bool ringbuffer_is_empty(const ringbuffer_t *rb) {
    if (!rb) return true;
    return (rb->head == rb->tail);
}

bool ringbuffer_is_full(const ringbuffer_t *rb) {
    if (!rb) return true;
    return (ringbuffer_count(rb) >= RINGBUFFER_CAPACITY_PACKETS);
}

bool ringbuffer_push(ringbuffer_t *rb, const safepulse_packet_t *pkt) {
    uint32_t current_count;
    uint32_t index;

    if (!rb || !pkt) return false;

    current_count = ringbuffer_count(rb);
    if (current_count >= RINGBUFFER_CAPACITY_PACKETS) {
        /* Buffer full: drop packet and record drop count */
        rb->drop_count++;
        return false;
    }

    index = rb->head & RINGBUFFER_MASK;
    /* Direct structure copy into fixed buffer slot (Zero dynamic allocation) */
    rb->packets[index] = *pkt;

    /* Memory barrier / sequencing */
    rb->head++;
    rb->total_pushed++;
    return true;
}

bool ringbuffer_pop(ringbuffer_t *rb, safepulse_packet_t *out_pkt) {
    uint32_t index;

    if (!rb || !out_pkt) return false;
    if (ringbuffer_is_empty(rb)) {
        return false;
    }

    index = rb->tail & RINGBUFFER_MASK;
    *out_pkt = rb->packets[index];

    rb->tail++;
    return true;
}
