#include "linter.h"
#include <string.h>
#include <math.h>

void linter_init(linter_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(linter_state_t));
}

void linter_inspect_packet(linter_state_t *state, safepulse_packet_t *pkt) {
    uint32_t expected_crc;
    sensor_track_state_t *track;
    double min_bound = 0.0, max_bound = 100.0, max_jerk = 100.0;
    double dt_sec = 0.0, delta_val = 0.0, rate = 0.0;
    float current_score = 100.0f;
    uint32_t violation = VIOLATION_NONE;

    if (!state || !pkt) return;

    state->total_inspected++;

    /* 1. Magic check */
    if (pkt->magic != SAFEPULSE_MAGIC) {
        pkt->violation_code = VIOLATION_CRC_CORRUPTED;
        pkt->safety_score = 0.0f;
        pkt->status_flags |= STATUS_FLAG_CRITICAL;
        state->total_violations++;
        return;
    }

    /* 2. CRC32 Checksum Verification */
    expected_crc = safepulse_crc32((const uint8_t*)pkt, offsetof(safepulse_packet_t, crc32));
    if (pkt->crc32 != expected_crc) {
        pkt->violation_code = VIOLATION_CRC_CORRUPTED;
        pkt->safety_score = 0.0f;
        pkt->status_flags |= STATUS_FLAG_CRITICAL;
        state->total_violations++;
        return;
    }

    /* 3. Sensor threshold configuration */
    switch (pkt->sensor_type) {
        case SENSOR_TYPE_STEERING:
            min_bound = LIMIT_STEERING_MIN;
            max_bound = LIMIT_STEERING_MAX;
            max_jerk  = LIMIT_STEERING_JERK;
            break;
        case SENSOR_TYPE_BRAKE:
            min_bound = LIMIT_BRAKE_MIN;
            max_bound = LIMIT_BRAKE_MAX;
            max_jerk  = LIMIT_BRAKE_JERK;
            break;
        case SENSOR_TYPE_THROTTLE:
            min_bound = LIMIT_THROTTLE_MIN;
            max_bound = LIMIT_THROTTLE_MAX;
            max_jerk  = LIMIT_THROTTLE_JERK;
            break;
        default:
            min_bound = -1000.0;
            max_bound =  1000.0;
            max_jerk  =  5000.0;
            break;
    }

    /* 4. Physical Out-of-Bounds Check */
    if (pkt->val_primary < min_bound || pkt->val_primary > max_bound) {
        violation = VIOLATION_OUT_OF_BOUNDS;
        current_score -= 50.0f;
        pkt->status_flags |= STATUS_FLAG_CRITICAL;
    }

    /* 5. Historical Rate-of-Change (Jerk) and Deadline Check */
    if (pkt->sensor_type < MAX_TRACKED_SENSORS) {
        track = &state->sensors[pkt->sensor_type];
        if (track->initialized) {
            if (pkt->timestamp_ns > track->last_timestamp_ns) {
                uint64_t dt_ns = pkt->timestamp_ns - track->last_timestamp_ns;

                /* Deadline Miss check */
                if (dt_ns > LIMIT_DEADLINE_NS) {
                    if (violation == VIOLATION_NONE) {
                        violation = VIOLATION_DEADLINE_MISS;
                    }
                    current_score -= 30.0f;
                    pkt->status_flags |= STATUS_FLAG_WARNING;
                }

                /* Jerk / Rate of change check (enforce minimum 5ms window to prevent scheduler jitter singularity) */
                if (dt_ns >= 5000000ULL) {
                    dt_sec = (double)dt_ns / 1000000000.0;
                    delta_val = fabs(pkt->val_primary - track->last_primary_val);
                    rate = delta_val / dt_sec;

                    if (rate > max_jerk) {
                        if (violation == VIOLATION_NONE) {
                            violation = VIOLATION_JERK_EXCEEDED;
                        }
                        current_score -= 40.0f;
                        pkt->status_flags |= STATUS_FLAG_CRITICAL;
                    }
                }
            }
        }

        /* Update tracked history */
        track->last_timestamp_ns = pkt->timestamp_ns;
        track->last_primary_val  = pkt->val_primary;
        track->last_seq_num      = pkt->seq_num;
        track->initialized       = true;
    }

    /* 6. Multi-Metric Plausibility Check (Sensor Contradiction) */
    if (pkt->sensor_type == SENSOR_TYPE_THROTTLE) {
        double throttle = pkt->val_primary;
        double speed = pkt->val_secondary;
        /* Unrealistic state: Extreme throttle with vehicle stopped or reversed without brake */
        if (throttle > 85.0 && speed < 1.0) {
            violation = VIOLATION_PLAUSIBILITY;
            current_score -= 35.0f;
            pkt->status_flags |= STATUS_FLAG_CRITICAL;
        }
    }

    /* 7. Score clamping */
    if (current_score < 0.0f) current_score = 0.0f;
    pkt->safety_score = current_score;
    pkt->violation_code = violation;

    if (violation != VIOLATION_NONE) {
        state->total_violations++;
    }

    /* Recompute CRC with the evaluated fields */
    pkt->crc32 = safepulse_crc32((const uint8_t*)pkt, offsetof(safepulse_packet_t, crc32));
}
