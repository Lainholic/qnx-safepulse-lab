#include "safe_state_fsm.h"
#include <stdio.h>
#include <math.h>

#define FAULTS_TO_DEGRADED   1
#define FAULTS_TO_LIMP_HOME  3
#define FAULTS_TO_ESTOP      5
#define SHADOW_XOR_KEY       0x5A5A5A5AU

static inline uint32_t calc_fsm_checksum(const safe_state_fsm_t *fsm) {
    return (uint32_t)fsm->current_state ^
           (uint32_t)fsm->shadow_state ^
           fsm->consecutive_faults ^
           (uint32_t)(fsm->state_entered_ns & 0xFFFFFFFFU) ^
           (fsm->is_latched ? 0x12345678U : 0U) ^
           SHADOW_XOR_KEY;
}

void safe_state_fsm_init(safe_state_fsm_t *fsm, uint64_t start_time_ns) {
    if (!fsm) return;
    fsm->current_state = ASIL_STATE_NORMAL;
    fsm->shadow_state = (asil_safe_state_t)(~ASIL_STATE_NORMAL);
    fsm->consecutive_faults = 0;
    fsm->state_entered_ns = 0;
    fsm->last_fault_timestamp_ns = 0;
    fsm->last_packet_timestamp_ns = 0;
    fsm->fsm_start_time_ns = start_time_ns;
    fsm->starvation_limit_ns = STARVATION_LIMIT_NS;
    fsm->total_degradations = 0;
    fsm->total_emergency_stops = 0;
    fsm->is_latched = false;
    fsm->ram_integrity_checksum = calc_fsm_checksum(fsm);
}

void safe_state_fsm_restore(safe_state_fsm_t *fsm, asil_safe_state_t state, uint64_t entered_ns, bool latched) {
    if (!fsm) return;
    fsm->current_state = state;
    fsm->shadow_state = (asil_safe_state_t)(~state);
    fsm->consecutive_faults = (state == ASIL_STATE_EMERGENCY_STOP) ? FAULTS_TO_ESTOP : 0;
    fsm->state_entered_ns = entered_ns;
    fsm->last_fault_timestamp_ns = entered_ns;
    fsm->last_packet_timestamp_ns = entered_ns;
    fsm->fsm_start_time_ns = entered_ns;
    fsm->starvation_limit_ns = STARVATION_LIMIT_NS;
    fsm->total_degradations = (state == ASIL_STATE_DEGRADED || state == ASIL_STATE_LIMP_HOME) ? 1 : 0;
    fsm->total_emergency_stops = (state == ASIL_STATE_EMERGENCY_STOP) ? 1 : 0;
    fsm->is_latched = latched;
    fsm->ram_integrity_checksum = calc_fsm_checksum(fsm);
    printf("[SafePulse FSM] Restored persistent safe state across crash: %s (Latched: %s)\n",
           safe_state_to_string(state), latched ? "TRUE" : "FALSE");
}

void safe_state_fsm_force_estop(safe_state_fsm_t *fsm, uint64_t timestamp_ns) {
    if (!fsm) return;
    fsm->current_state = ASIL_STATE_EMERGENCY_STOP;
    fsm->shadow_state = (asil_safe_state_t)(~ASIL_STATE_EMERGENCY_STOP);
    fsm->state_entered_ns = timestamp_ns;
    fsm->last_fault_timestamp_ns = timestamp_ns;
    fsm->is_latched = true; /* Permanent lock in E-STOP */
    fsm->total_emergency_stops++;
    fsm->ram_integrity_checksum = calc_fsm_checksum(fsm);
}

void safe_state_fsm_check_starvation(safe_state_fsm_t *fsm, uint64_t current_time_ns) {
    if (!fsm) return;
    if (fsm->current_state == ASIL_STATE_EMERGENCY_STOP) return;

    /* Only evaluate starvation after receiving at least one sensor packet (Grace period during boot) */
    if (fsm->last_packet_timestamp_ns == 0) return;

    /* Enforce configured starvation limit (default STARVATION_LIMIT_NS = 50ms) */
    uint64_t limit_ns = (fsm->starvation_limit_ns > 0ULL) ? fsm->starvation_limit_ns : STARVATION_LIMIT_NS;
    if (current_time_ns > fsm->last_packet_timestamp_ns && 
        (current_time_ns - fsm->last_packet_timestamp_ns) >= limit_ns) {
        printf("[SafePulse FSM] CRITICAL: Sensor packet starvation detected (Silence >= %llu ms). Forcing EMERGENCY_STOP!\n",
               (unsigned long long)(limit_ns / 1000000ULL));
        safe_state_fsm_force_estop(fsm, current_time_ns);
    }
}

asil_safe_state_t safe_state_fsm_step(
    safe_state_fsm_t *fsm,
    uint32_t violation_code,
    float safety_score,
    uint64_t timestamp_ns
) {
    if (!fsm) return ASIL_STATE_EMERGENCY_STOP;

    if (timestamp_ns > fsm->last_packet_timestamp_ns) {
        fsm->last_packet_timestamp_ns = timestamp_ns;
    }

    /* 1. ASIL-D RAM Integrity Diagnostics: Detect Transient Bit-Flips */
    if (fsm->ram_integrity_checksum != calc_fsm_checksum(fsm) ||
        (uint32_t)fsm->current_state != (uint32_t)(~fsm->shadow_state)) {
        safe_state_fsm_force_estop(fsm, timestamp_ns);
        return ASIL_STATE_EMERGENCY_STOP;
    }

    /* 2. Permanent Lock Check: If already in EMERGENCY_STOP, nothing can downgrade it */
    if (fsm->current_state == ASIL_STATE_EMERGENCY_STOP) {
        return ASIL_STATE_EMERGENCY_STOP;
    }

    bool has_violation = (violation_code != 0 || safety_score < 70.0f || isnan(safety_score));

    if (has_violation) {
        fsm->consecutive_faults++;
        fsm->last_fault_timestamp_ns = timestamp_ns;

        /* Critical CRC corruption or Plausibility contradiction triggers immediate E-STOP */
        if (violation_code == 0x00001004 || violation_code == 0x00001005) {
            safe_state_fsm_force_estop(fsm, timestamp_ns);
            return fsm->current_state;
        }

        /* ISO 26262 FTTI Enforcement: If degraded for >10ms without resolution, escalate to ESTOP */
        if (fsm->current_state > ASIL_STATE_NORMAL && fsm->current_state < ASIL_STATE_EMERGENCY_STOP) {
            if (fsm->state_entered_ns > 0 && (timestamp_ns - fsm->state_entered_ns) >= FTTI_THRESHOLD_NS) {
                safe_state_fsm_force_estop(fsm, timestamp_ns);
                return fsm->current_state;
            }
        }

        /* Gradual escalation based on fault accumulation (Upward transitions ALWAYS permitted) */
        if (fsm->consecutive_faults >= FAULTS_TO_ESTOP) {
            safe_state_fsm_force_estop(fsm, timestamp_ns);
        } else if (fsm->consecutive_faults >= FAULTS_TO_LIMP_HOME) {
            if (fsm->current_state < ASIL_STATE_LIMP_HOME) {
                fsm->current_state = ASIL_STATE_LIMP_HOME;
                fsm->is_latched = true; /* Block downward recovery */
                fsm->total_degradations++;
                fsm->state_entered_ns = timestamp_ns;
            }
        } else if (fsm->consecutive_faults >= FAULTS_TO_DEGRADED) {
            if (fsm->current_state < ASIL_STATE_DEGRADED) {
                fsm->current_state = ASIL_STATE_DEGRADED;
                fsm->total_degradations++;
                fsm->state_entered_ns = timestamp_ns;
            }
        }
    } else {
        /* Recovery path: ONLY allowed if NOT latched (LIMP_HOME cannot downgrade to DEGRADED/NORMAL) */
        if (!fsm->is_latched) {
            if (fsm->consecutive_faults > 0) {
                fsm->consecutive_faults--;
            }

            if (fsm->consecutive_faults == 0 && fsm->current_state == ASIL_STATE_DEGRADED) {
                fsm->current_state = ASIL_STATE_NORMAL;
                fsm->state_entered_ns = timestamp_ns;
            }
        }
    }

    fsm->shadow_state = (asil_safe_state_t)(~fsm->current_state);
    fsm->ram_integrity_checksum = calc_fsm_checksum(fsm);
    return fsm->current_state;
}

actuator_limits_t safe_state_get_limits(asil_safe_state_t state) {
    actuator_limits_t lim;
    switch (state) {
        case ASIL_STATE_NORMAL:
            lim.max_steering_angle = 540.0;
            lim.max_throttle_pct = 100.0;
            lim.min_brake_bar = 0.0;
            lim.fail_safe_active = false;
            break;
        case ASIL_STATE_DEGRADED:
            lim.max_steering_angle = 180.0;
            lim.max_throttle_pct = 60.0;
            lim.min_brake_bar = 0.0;
            lim.fail_safe_active = true;
            break;
        case ASIL_STATE_LIMP_HOME:
            lim.max_steering_angle = 90.0;
            lim.max_throttle_pct = 30.0;
            lim.min_brake_bar = 5.0;
            lim.fail_safe_active = true;
            break;
        case ASIL_STATE_EMERGENCY_STOP:
        default:
            lim.max_steering_angle = 0.0;
            lim.max_throttle_pct = 0.0;
            lim.min_brake_bar = 100.0;
            lim.fail_safe_active = true;
            break;
    }
    return lim;
}

const char *safe_state_to_string(asil_safe_state_t state) {
    switch (state) {
        case ASIL_STATE_NORMAL:         return "ASIL_STATE_NORMAL";
        case ASIL_STATE_DEGRADED:       return "ASIL_STATE_DEGRADED";
        case ASIL_STATE_LIMP_HOME:      return "ASIL_STATE_LIMP_HOME";
        case ASIL_STATE_EMERGENCY_STOP: return "ASIL_STATE_EMERGENCY_STOP";
        default:                        return "UNKNOWN_STATE";
    }
}

void actuator_slew_init(actuator_slew_state_t *slew) {
    if (!slew) return;
    slew->last_clamped_steering = 0.0;
    slew->last_clamped_throttle = 0.0;
    slew->last_clamped_brake = 0.0;
    slew->last_steer_timestamp_ns = 0;
    slew->last_throttle_timestamp_ns = 0;
    slew->last_brake_timestamp_ns = 0;
}

double actuator_slew_clamp_steering(
    actuator_slew_state_t *slew,
    double target_angle,
    double max_angle_limit,
    uint64_t timestamp_ns
) {
    if (!slew) return target_angle;
    if (!isfinite(target_angle)) target_angle = 0.0;

    /* Static limit boundary check */
    if (target_angle > max_angle_limit) target_angle = max_angle_limit;
    if (target_angle < -max_angle_limit) target_angle = -max_angle_limit;

    /* First initialization */
    if (slew->last_steer_timestamp_ns == 0) {
        slew->last_clamped_steering = target_angle;
        slew->last_steer_timestamp_ns = timestamp_ns;
        return target_angle;
    }

    /* Zero or backwards elapsed time: Reset tracking or advance with 1ms epsilon */
    if (timestamp_ns <= slew->last_steer_timestamp_ns) {
        slew->last_steer_timestamp_ns = timestamp_ns;
        return target_angle;
    }

    double dt_sec = (double)(timestamp_ns - slew->last_steer_timestamp_ns) / 1000000000.0;
    if (dt_sec <= 0.0) dt_sec = 0.001;
    if (dt_sec > 0.5) dt_sec = 0.5;

    double max_delta = MAX_STEERING_SLEW_RATE_DEG_S * dt_sec;
    double delta = target_angle - slew->last_clamped_steering;

    if (delta > max_delta) {
        target_angle = slew->last_clamped_steering + max_delta;
    } else if (delta < -max_delta) {
        target_angle = slew->last_clamped_steering - max_delta;
    }

    /* Final static boundary enforcement: slew must never violate hard safety ceiling */
    if (target_angle > max_angle_limit) target_angle = max_angle_limit;
    if (target_angle < -max_angle_limit) target_angle = -max_angle_limit;

    slew->last_clamped_steering = target_angle;
    slew->last_steer_timestamp_ns = timestamp_ns;
    return target_angle;
}

double actuator_slew_clamp_throttle(
    actuator_slew_state_t *slew,
    double target_throttle,
    double max_throttle_limit,
    uint64_t timestamp_ns
) {
    if (!slew) return target_throttle;
    if (!isfinite(target_throttle) || target_throttle < 0.0) target_throttle = 0.0;

    /* Static limit boundary check */
    if (target_throttle > max_throttle_limit) target_throttle = max_throttle_limit;

    /* First initialization */
    if (slew->last_throttle_timestamp_ns == 0) {
        slew->last_clamped_throttle = target_throttle;
        slew->last_throttle_timestamp_ns = timestamp_ns;
        return target_throttle;
    }

    /* Zero or backwards elapsed time: Hold previous clamped value (anti-bypass) */
    if (timestamp_ns <= slew->last_throttle_timestamp_ns) {
        return slew->last_clamped_throttle;
    }

    double dt_sec = (double)(timestamp_ns - slew->last_throttle_timestamp_ns) / 1000000000.0;
    if (dt_sec <= 0.0) dt_sec = 0.001;
    if (dt_sec > 0.5) dt_sec = 0.5;

    double max_delta = MAX_THROTTLE_SLEW_RATE_PCT_S * dt_sec;
    double delta = target_throttle - slew->last_clamped_throttle;

    if (delta > max_delta) {
        target_throttle = slew->last_clamped_throttle + max_delta;
    } else if (delta < -max_delta) {
        target_throttle = slew->last_clamped_throttle - max_delta;
    }

    /* Final static boundary enforcement: slew must never violate hard safety ceiling */
    if (target_throttle > max_throttle_limit) target_throttle = max_throttle_limit;
    if (target_throttle < 0.0) target_throttle = 0.0;

    slew->last_clamped_throttle = target_throttle;
    slew->last_throttle_timestamp_ns = timestamp_ns;
    return target_throttle;
}
