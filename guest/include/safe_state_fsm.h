/**
 * SafePulse ISO 26262 ASIL-D Safe State Finite State Machine (FSM)
 * Features Dual-Rail Redundancy & RAM Bit-Flip Integrity Checksum.
 */

#ifndef SAFEPULSE_SAFE_STATE_FSM_H
#define SAFEPULSE_SAFE_STATE_FSM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ASIL_STATE_NORMAL         = 0, /* Fully operational nominal state */
    ASIL_STATE_DEGRADED       = 1, /* Minor fault: Warning active, 70% power cap */
    ASIL_STATE_LIMP_HOME      = 2, /* Persistent sensor fault: 40km/h cap, cautious assist */
    ASIL_STATE_EMERGENCY_STOP = 3  /* Catastrophic fault: Zero throttle, 100% max deceleration */
} asil_safe_state_t;

typedef struct {
    asil_safe_state_t current_state;
    asil_safe_state_t shadow_state;      /* Inverted redundant copy for Single-Event-Upset detection */
    uint32_t consecutive_faults;
    uint64_t state_entered_ns;
    uint64_t last_fault_timestamp_ns;
    uint32_t total_degradations;
    uint32_t total_emergency_stops;
    uint32_t ram_integrity_checksum;   /* XOR checksum over FSM state */
} safe_state_fsm_t;

typedef struct {
    double max_steering_angle;
    double max_throttle_pct;
    double min_brake_bar;
    bool   fail_safe_active;
} actuator_limits_t;

void safe_state_fsm_init(safe_state_fsm_t *fsm);

asil_safe_state_t safe_state_fsm_step(
    safe_state_fsm_t *fsm,
    uint32_t violation_code,
    float safety_score,
    uint64_t timestamp_ns
);

actuator_limits_t safe_state_get_limits(asil_safe_state_t state);
const char *safe_state_to_string(asil_safe_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_SAFE_STATE_FSM_H */
