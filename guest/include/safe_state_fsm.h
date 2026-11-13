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

#define FTTI_THRESHOLD_NS    (10 * 1000 * 1000ULL) /* 10ms ISO 26262 Fault Tolerant Time Interval */
#define STARVATION_LIMIT_NS  (50 * 1000 * 1000ULL) /* 50ms Hard Starvation Timeout (reduced 3x from 150ms, zero false positives over 10-min QEMU soak) */

typedef enum {
    ASIL_STATE_NORMAL         = 0, /* Fully operational nominal state */
    ASIL_STATE_DEGRADED       = 1, /* Minor fault: 60% throttle, 50 km/h, limited steering rate */
    ASIL_STATE_LIMP_HOME      = 2, /* Persistent fault: 30% throttle, 30 km/h, tight steering, latched */
    ASIL_STATE_EMERGENCY_STOP = 3  /* Catastrophic fault: Zero throttle, 100% max braking, permanent unrecoverable latch */
} asil_safe_state_t;

typedef struct {
    asil_safe_state_t current_state;
    asil_safe_state_t shadow_state;        /* Inverted redundant copy for Single-Event-Upset detection */
    uint32_t consecutive_faults;
    uint64_t state_entered_ns;
    uint64_t last_fault_timestamp_ns;
    uint64_t last_packet_timestamp_ns;     /* Heartbeat tracking for starvation detection */
    uint64_t fsm_start_time_ns;            /* System start timestamp for first-packet timeout */
    uint64_t starvation_limit_ns;          /* Configurable starvation timeout (default STARVATION_LIMIT_NS) */
    uint32_t total_degradations;
    uint32_t total_emergency_stops;
    bool     is_latched;                   /* True if downward recovery is blocked */
    uint32_t ram_integrity_checksum;       /* XOR checksum over FSM state */
    bool     starvation_estop;             /* True if the starvation watchdog forced the current EMERGENCY_STOP */
} safe_state_fsm_t;

typedef struct {
    double max_steering_angle;
    double max_steering_slew_deg_s;
    double max_throttle_pct;
    double max_speed_kmh;
    double min_brake_bar;
    bool   fail_safe_active;
} actuator_limits_t;

/* Slew-Rate Limiter for Smooth Safe State Actuator Transition (ISO 26262-4 / 6) */
#define MAX_STEERING_SLEW_RATE_DEG_S   250.0  /* Max 250 deg/s return to center */
#define MAX_THROTTLE_SLEW_RATE_PCT_S   100.0  /* Max 100 %/s ramp down */
#define MAX_BRAKE_SLEW_RATE_BAR_S      150.0  /* Max 150 bar/s ramp up to prevent passenger jerk */

typedef struct {
    double last_clamped_steering;
    double last_clamped_throttle;
    double last_clamped_brake;
    double last_clamped_speed;
    uint64_t last_steer_timestamp_ns;    /* Channel-isolated steer timestamp */
    uint64_t last_throttle_timestamp_ns; /* Channel-isolated throttle timestamp */
    uint64_t last_brake_timestamp_ns;    /* Channel-isolated brake timestamp */
    uint64_t last_speed_timestamp_ns;    /* Channel-isolated speed timestamp */
} actuator_slew_state_t;

void safe_state_fsm_init(safe_state_fsm_t *fsm, uint64_t start_time_ns);
void safe_state_fsm_restore(safe_state_fsm_t *fsm, asil_safe_state_t state, uint64_t entered_ns, bool latched);
void actuator_slew_init(actuator_slew_state_t *slew);

double actuator_slew_clamp_steering(
    actuator_slew_state_t *slew,
    double target_angle,
    double max_angle_limit,
    double max_slew_deg_s,
    uint64_t timestamp_ns
);

double actuator_slew_clamp_speed(
    actuator_slew_state_t *slew,
    double target_speed,
    double max_speed_kmh,
    uint64_t timestamp_ns
);

double actuator_slew_clamp_throttle(
    actuator_slew_state_t *slew,
    double target_throttle,
    double max_throttle_limit,
    uint64_t timestamp_ns
);

asil_safe_state_t safe_state_fsm_step(
    safe_state_fsm_t *fsm,
    uint32_t violation_code,
    float safety_score,
    uint64_t timestamp_ns
);

void safe_state_fsm_check_starvation(safe_state_fsm_t *fsm, uint64_t current_time_ns);
void safe_state_fsm_force_estop(safe_state_fsm_t *fsm, uint64_t timestamp_ns);

actuator_limits_t safe_state_get_limits(asil_safe_state_t state);
const char *safe_state_to_string(asil_safe_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_SAFE_STATE_FSM_H */
