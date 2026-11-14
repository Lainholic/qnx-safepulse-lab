/**
 * @file safepulse_hal.h
 * @brief Automotive Hardware Abstraction Layer (HAL) for CAN / CAN FD / SPI Controllers
 * @standard ISO 26262-5:2018 (Hardware Architecture), ISO 11898-1:2015 (CAN FD)
 * 
 * Provides unified register-level abstraction and fault-injection capabilities
 * for automotive microcontrollers (e.g. Bosch M_CAN, NXP FlexCAN, Infineon MultiCAN+).
 */

#ifndef SAFEPULSE_HAL_H
#define SAFEPULSE_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAN FD Frame Constraints */
#define HAL_CAN_MAX_PAYLOAD_BYTES  64U
#define HAL_CAN_MAX_CHANNELS        4U

/* Bus State Machine (ISO 11898-1 Section 12.1.4) */
typedef enum {
    HAL_BUS_STATE_ACTIVE   = 0, /* Error Active: TEC < 128, REC < 128 (Normal transmission) */
    HAL_BUS_STATE_WARNING  = 1, /* Error Warning: TEC >= 96 or REC >= 96 */
    HAL_BUS_STATE_PASSIVE  = 2, /* Error Passive: TEC >= 128 or REC >= 128 */
    HAL_BUS_STATE_BUS_OFF  = 3  /* Bus-Off: TEC > 255 (Transmitter node detached from bus) */
} hal_bus_state_t;

/* Transceiver Fault Modes */
typedef enum {
    HAL_FAULT_NONE         = 0x00,
    HAL_FAULT_BIT_STUFF    = 0x01, /* Bit stuffing error on physical bus */
    HAL_FAULT_CRC_CORRUPT  = 0x02, /* Frame CRC mismatch */
    HAL_FAULT_FORM_ERROR   = 0x04, /* Delimiter or EOF format error */
    HAL_FAULT_ACK_DELIMIT  = 0x08, /* Missing Acknowledge slot bit */
    HAL_FAULT_BUS_OFF_TRIP = 0x10, /* Force TEC > 255 to trip Bus-Off state */
    HAL_FAULT_JITTER_SPIKE = 0x20  /* Physical clock phase jitter (> 200ns) */
} hal_fault_injection_t;

/* CAN FD Hardware Frame Structure (64-byte payload) */
typedef struct {
    uint32_t can_id;                            /* 11-bit Standard or 29-bit Extended CAN ID */
    uint8_t  channel;                           /* Physical controller channel (0..3) */
    bool     is_extended;                       /* True: 29-bit ID, False: 11-bit ID */
    bool     is_can_fd;                         /* True: CAN FD format (EDL/FDF bit = 1) */
    bool     bitrate_switch;                    /* True: BRS bit = 1 (Fast data phase enabled) */
    bool     esi_error_state;                   /* ESI: Error State Indicator */
    uint8_t  dlc;                               /* Data Length Code (0..15 -> 0..64 bytes) */
    uint8_t  payload_len;                       /* Exact byte count (0..64) */
    uint8_t  payload[HAL_CAN_MAX_PAYLOAD_BYTES];/* 64-byte raw payload */
    uint64_t hw_timestamp_ns;                   /* Hardware timer capture timestamp */
    uint32_t crc21;                             /* Hardware computed CAN FD CRC-17/CRC-21 */
} hal_can_frame_t;

/* Controller Operational & Error Statistics */
typedef struct {
    hal_bus_state_t bus_state;
    uint16_t tec;                               /* Transmit Error Counter (0..256) */
    uint16_t rec;                               /* Receive Error Counter (0..256) */
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t bus_off_count;
    uint32_t bus_off_recoveries;
    uint32_t dropped_frames;
    uint32_t crc_errors;
    uint32_t bit_stuff_errors;
    double   bus_load_pct;                      /* Current physical bus saturation (0.0 .. 100.0%) */
} hal_controller_stats_t;

/* HAL Hardware Controller Configuration */
typedef struct {
    uint32_t nominal_bitrate;                   /* e.g. 500,000 bps (500 kbps arbitration) */
    uint32_t data_bitrate;                      /* e.g. 2,000,000 or 5,000,000 bps (2..5 Mbps data) */
    uint8_t  sample_point_nom_pct;              /* e.g. 80% nominal sample point */
    uint8_t  sample_point_data_pct;             /* e.g. 70% data sample point */
    bool     auto_bus_off_recovery;             /* ISO 11898-1 automatic 128x11-bit sequence recovery */
    bool     fd_enabled;
} hal_can_config_t;

/* Controller Life Cycle & Data Exchange API */
int  hal_can_init(uint8_t channel, const hal_can_config_t *config);
int  hal_can_deinit(uint8_t channel);
int  hal_can_transmit(uint8_t channel, const hal_can_frame_t *frame);
int  hal_can_receive(uint8_t channel, hal_can_frame_t *frame, uint32_t timeout_us);
int  hal_can_get_stats(uint8_t channel, hal_controller_stats_t *out_stats);
int  hal_can_inject_fault(uint8_t channel, hal_fault_injection_t fault_mask);
int  hal_can_clear_faults(uint8_t channel);
int  hal_can_trigger_bus_off_recovery(uint8_t channel);

/* DLC to byte length conversion table (ISO 11898-1:2015) */
static inline uint8_t hal_dlc_to_bytes(uint8_t dlc) {
    static const uint8_t dlc_table[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
    };
    return (dlc < 16) ? dlc_table[dlc] : 64U;
}

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_HAL_H */
