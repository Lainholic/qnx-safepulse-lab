/**
 * @file safepulse_hal_can.c
 * @brief Implementation of Automotive CAN FD Hardware Abstraction Layer
 * @standard ISO 26262-5:2018, ISO 11898-1:2015
 *
 * Implements hardware mailbox emulation, Bosch M_CAN/FlexCAN register state machines,
 * error counter tracking (TEC/REC), and automatic 128x11-bit Bus-Off recovery sequences.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "safepulse_hal.h"
#include "safepulse.h"

#define HAL_RING_CAPACITY 64U

typedef struct {
    hal_can_frame_t ring[HAL_RING_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} hal_fifo_t;

typedef struct {
    bool is_initialized;
    hal_can_config_t config;
    hal_controller_stats_t stats;
    hal_fault_injection_t active_fault;
    hal_fifo_t rx_fifo;
    hal_fifo_t tx_fifo;
    uint64_t last_bus_off_recovery_ns;
} hal_channel_ctrl_t;

static hal_channel_ctrl_t g_channels[HAL_CAN_MAX_CHANNELS];

static uint64_t hal_get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static uint32_t hal_compute_can_crc(const uint8_t *data, size_t len) {
    /* Standard CAN FD CRC-21 Polynomial 0x302899 representation */
    uint32_t crc = 0x000000;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint32_t)data[i] << 13);
        for (int b = 0; b < 8; b++) {
            if (crc & 0x100000) {
                crc = (crc << 1) ^ 0x302899;
            } else {
                crc = (crc << 1);
            }
            crc &= 0x1FFFFF;
        }
    }
    return crc;
}

int hal_can_init(uint8_t channel, const hal_can_config_t *config) {
    if (channel >= HAL_CAN_MAX_CHANNELS || !config) {
        return -EINVAL;
    }
    hal_channel_ctrl_t *ctrl = &g_channels[channel];
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->config = *config;
    ctrl->is_initialized = true;
    ctrl->stats.bus_state = HAL_BUS_STATE_ACTIVE;
    ctrl->stats.tec = 0;
    ctrl->stats.rec = 0;
    ctrl->stats.bus_load_pct = 5.0; /* Baseline nominal idle load */
    return 0;
}

int hal_can_deinit(uint8_t channel) {
    if (channel >= HAL_CAN_MAX_CHANNELS) return -EINVAL;
    g_channels[channel].is_initialized = false;
    return 0;
}

int hal_can_inject_fault(uint8_t channel, hal_fault_injection_t fault_mask) {
    if (channel >= HAL_CAN_MAX_CHANNELS) return -EINVAL;
    g_channels[channel].active_fault |= fault_mask;
    if (fault_mask & HAL_FAULT_BUS_OFF_TRIP) {
        g_channels[channel].stats.tec = 256;
        g_channels[channel].stats.bus_state = HAL_BUS_STATE_BUS_OFF;
        g_channels[channel].stats.bus_off_count++;
    }
    return 0;
}

int hal_can_clear_faults(uint8_t channel) {
    if (channel >= HAL_CAN_MAX_CHANNELS) return -EINVAL;
    g_channels[channel].active_fault = HAL_FAULT_NONE;
    return 0;
}

int hal_can_trigger_bus_off_recovery(uint8_t channel) {
    if (channel >= HAL_CAN_MAX_CHANNELS) return -EINVAL;
    hal_channel_ctrl_t *ctrl = &g_channels[channel];
    if (ctrl->stats.bus_state != HAL_BUS_STATE_BUS_OFF) {
        return 0;
    }
    /* ISO 11898-1: Simulated 128 occurrences of 11 consecutive recessive bits */
    ctrl->stats.tec = 0;
    ctrl->stats.rec = 0;
    ctrl->stats.bus_state = HAL_BUS_STATE_ACTIVE;
    ctrl->stats.bus_off_recoveries++;
    ctrl->last_bus_off_recovery_ns = hal_get_monotonic_ns();
    ctrl->active_fault &= ~HAL_FAULT_BUS_OFF_TRIP;
    return 0;
}

int hal_can_transmit(uint8_t channel, const hal_can_frame_t *frame) {
    if (channel >= HAL_CAN_MAX_CHANNELS || !frame) return -EINVAL;
    hal_channel_ctrl_t *ctrl = &g_channels[channel];
    if (!ctrl->is_initialized) return -ENODEV;

    /* Hardware Bus-Off transmission lock */
    if (ctrl->stats.bus_state == HAL_BUS_STATE_BUS_OFF) {
        if (ctrl->config.auto_bus_off_recovery) {
            hal_can_trigger_bus_off_recovery(channel);
        } else {
            return -ENETDOWN;
        }
    }

    /* Simulate Physical Fault Injections */
    if (ctrl->active_fault & HAL_FAULT_CRC_CORRUPT) {
        ctrl->stats.crc_errors++;
        ctrl->stats.tec += 8;
    }
    if (ctrl->active_fault & HAL_FAULT_BIT_STUFF) {
        ctrl->stats.bit_stuff_errors++;
        ctrl->stats.tec += 8;
    }

    /* Update ISO 11898 Error State thresholds */
    if (ctrl->stats.tec >= 256) {
        ctrl->stats.bus_state = HAL_BUS_STATE_BUS_OFF;
        ctrl->stats.bus_off_count++;
        return -ENETDOWN;
    } else if (ctrl->stats.tec >= 128) {
        ctrl->stats.bus_state = HAL_BUS_STATE_PASSIVE;
    } else if (ctrl->stats.tec >= 96) {
        ctrl->stats.bus_state = HAL_BUS_STATE_WARNING;
    } else {
        ctrl->stats.bus_state = HAL_BUS_STATE_ACTIVE;
    }

    /* Enqueue to internal Rx FIFO of recipient nodes */
    hal_fifo_t *fifo = &ctrl->rx_fifo;
    if (fifo->count < HAL_RING_CAPACITY) {
        hal_can_frame_t stamped = *frame;
        stamped.hw_timestamp_ns = hal_get_monotonic_ns();
        stamped.crc21 = hal_compute_can_crc(stamped.payload, stamped.payload_len);
        fifo->ring[fifo->head] = stamped;
        fifo->head = (fifo->head + 1U) % HAL_RING_CAPACITY;
        fifo->count++;
        ctrl->stats.tx_frames++;
        ctrl->stats.rx_frames++;
    } else {
        ctrl->stats.dropped_frames++;
        return -ENOBUFS;
    }

    return 0;
}

int hal_can_receive(uint8_t channel, hal_can_frame_t *frame, uint32_t timeout_us) {
    if (channel >= HAL_CAN_MAX_CHANNELS || !frame) return -EINVAL;
    hal_channel_ctrl_t *ctrl = &g_channels[channel];
    if (!ctrl->is_initialized) return -ENODEV;
    (void)timeout_us;

    hal_fifo_t *fifo = &ctrl->rx_fifo;
    if (fifo->count == 0) {
        return -EAGAIN;
    }

    *frame = fifo->ring[fifo->tail];
    fifo->tail = (fifo->tail + 1U) % HAL_RING_CAPACITY;
    fifo->count--;

    /* Hardware Error Decrement on successful receive (ISO 11898-1) */
    if (ctrl->stats.rec > 0) ctrl->stats.rec--;
    if (ctrl->stats.tec > 0) ctrl->stats.tec--;

    return 0;
}

int hal_can_get_stats(uint8_t channel, hal_controller_stats_t *out_stats) {
    if (channel >= HAL_CAN_MAX_CHANNELS || !out_stats) return -EINVAL;
    *out_stats = g_channels[channel].stats;
    return 0;
}
