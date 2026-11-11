#include "linter.h"
#include <string.h>
#include <math.h>

void linter_init(linter_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(linter_state_t));
    for (uint16_t i = 0; i < MAX_TRACKED_SENSORS; i++) {
        state->rules[i].min_bound   = -1000.0;
        state->rules[i].max_bound   =  1000.0;
        state->rules[i].max_jerk    =  5000.0;
        state->rules[i].deadline_ns = LIMIT_DEADLINE_NS;

        state->e2e_channels[i].data_id = (uint16_t)(SAFEPULSE_E2E_DATA_ID_BASE + i);
        state->e2e_channels[i].max_counter_jump = 2;
        state->e2e_channels[i].last_counter = 0;
        state->e2e_channels[i].initialized = false;
    }

    state->rules[SENSOR_TYPE_STEERING].min_bound = LIMIT_STEERING_MIN;
    state->rules[SENSOR_TYPE_STEERING].max_bound = LIMIT_STEERING_MAX;
    state->rules[SENSOR_TYPE_STEERING].max_jerk  = LIMIT_STEERING_JERK;

    state->rules[SENSOR_TYPE_BRAKE].min_bound    = LIMIT_BRAKE_MIN;
    state->rules[SENSOR_TYPE_BRAKE].max_bound    = LIMIT_BRAKE_MAX;
    state->rules[SENSOR_TYPE_BRAKE].max_jerk     = LIMIT_BRAKE_JERK;

    state->rules[SENSOR_TYPE_THROTTLE].min_bound = LIMIT_THROTTLE_MIN;
    state->rules[SENSOR_TYPE_THROTTLE].max_bound = LIMIT_THROTTLE_MAX;
    state->rules[SENSOR_TYPE_THROTTLE].max_jerk  = LIMIT_THROTTLE_JERK;
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

    /* 2b. AUTOSAR E2E Profile 1 Inline Verification (CRC-32 0xF4ACFB13 + 4-bit Alive Counter + Data ID) */
    if (pkt->reserved[3] == SAFEPULSE_E2E_MARKER && pkt->sensor_type < MAX_TRACKED_SENSORS) {
        uint16_t pkt_data_id = (uint16_t)pkt->reserved[0] | ((uint16_t)pkt->reserved[1] << 8);
        uint8_t  rolling_cnt = pkt->reserved[2];
        uint32_t tx_e2e_crc  = 0;
        memcpy(&tx_e2e_crc, &pkt->reserved[4], sizeof(uint32_t));

        e2e_status_t e2e_st = autosar_e2e_protect_verify(
            &state->e2e_channels[pkt->sensor_type],
            pkt_data_id,
            rolling_cnt,
            tx_e2e_crc,
            (const uint8_t *)pkt,
            SAFEPULSE_E2E_PAYLOAD_LEN
        );
        if (e2e_st != E2E_STATUS_OK && e2e_st != E2E_STATUS_INITIALIZING) {
            pkt->violation_code = autosar_e2e_status_to_violation(e2e_st);
            if (e2e_st == E2E_STATUS_DATA_CORRUPTED || e2e_st == E2E_STATUS_WRONG_DATA_ID) {
                pkt->safety_score = 0.0f;
                pkt->status_flags |= STATUS_FLAG_CRITICAL;
            } else {
                pkt->safety_score = 60.0f;
                pkt->status_flags |= STATUS_FLAG_WARNING;
            }
            state->total_violations++;
            pkt->crc32 = safepulse_crc32((const uint8_t*)pkt, offsetof(safepulse_packet_t, crc32));
            return;
        }
    }

    /* 3. Sensor threshold configuration (dynamic rules table with compile-time fallback) */
    uint64_t deadline_limit_ns = LIMIT_DEADLINE_NS;
    if (pkt->sensor_type < MAX_TRACKED_SENSORS) {
        min_bound         = state->rules[pkt->sensor_type].min_bound;
        max_bound         = state->rules[pkt->sensor_type].max_bound;
        max_jerk          = state->rules[pkt->sensor_type].max_jerk;
        deadline_limit_ns = state->rules[pkt->sensor_type].deadline_ns;
    } else {
        min_bound = -1000.0;
        max_bound =  1000.0;
        max_jerk  =  5000.0;
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
                if (dt_ns > deadline_limit_ns) {
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
