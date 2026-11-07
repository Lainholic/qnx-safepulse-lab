/**
 * SafePulse AUTOSAR E2E (End-to-End) Protection Profile 1 Specification
 * Compliant with ISO 26262 Part 6 ASIL-D Communication Protection.
 */

#ifndef SAFEPULSE_AUTOSAR_E2E_H
#define SAFEPULSE_AUTOSAR_E2E_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AUTOSAR E2E Profile 1 Status Codes */
typedef enum {
    E2E_STATUS_OK             = 0x00, /* Protection verified, message valid */
    E2E_STATUS_DATA_CORRUPTED = 0x01, /* CRC mismatch (Bus error/Bitflip) */
    E2E_STATUS_REPEATED       = 0x02, /* Alive counter repeated (Freeze/Replay) */
    E2E_STATUS_SEQUENCE_LOSS  = 0x03, /* Alive counter jumped (Dropped frame) */
    E2E_STATUS_WRONG_DATA_ID  = 0x04, /* Data ID mismatch (Masquerade) */
    E2E_STATUS_INITIALIZING   = 0x05  /* Sequence syncing */
} e2e_status_t;

/* AUTOSAR E2E Channel Configuration */
typedef struct {
    uint16_t data_id;           /* Unique 16-bit identifier per sensor stream */
    uint8_t  max_counter_jump;  /* Allowed gap before declaring packet loss (typical: 2) */
    uint8_t  last_counter;      /* Last received rolling counter (0..15) */
    bool     initialized;       /* Whether first packet received */
} e2e_channel_state_t;

/* AUTOSAR 8-bit CRC calculation (Polynomial 0x1D - SAE J1850) */
uint8_t autosar_crc8_sae_j1850(const uint8_t *data, size_t length);

/* Table-Driven Deterministic AUTOSAR 32-bit CRC calculation (Polynomial 0xF4ACFB13) */
uint32_t autosar_e2e_crc32(uint16_t data_id, const uint8_t *payload, size_t length);

/* E2E Verification Engine */
e2e_status_t autosar_e2e_protect_verify(
    e2e_channel_state_t *channel,
    uint16_t packet_data_id,
    uint8_t  rolling_counter,
    uint32_t transmitted_crc,
    const uint8_t *payload,
    size_t length
);

/* Bijective Mapping from E2E Status to ASIL-D Violation Code */
uint32_t autosar_e2e_status_to_violation(e2e_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_AUTOSAR_E2E_H */
