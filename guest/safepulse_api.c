/**
 * SafePulse Embedded Safety SDK Implementation
 * Encapsulates Linter Engine, AUTOSAR E2E Profile 1, and Safe State FSM Watchdog
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "safepulse_api.h"
#include "safepulse.h"
#include "linter.h"
#include "safe_state_fsm.h"
#include "autosar_e2e.h"

struct safepulse_ctx {
    linter_state_t        linter;
    safe_state_fsm_t      fsm;
    actuator_slew_state_t slew;
};

static uint64_t sdk_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

safepulse_ctx_t *safepulse_init(void) {
    safepulse_ctx_t *ctx = (safepulse_ctx_t *)calloc(1, sizeof(safepulse_ctx_t));
    if (!ctx) return NULL;

    linter_init(&ctx->linter);
    safe_state_fsm_init(&ctx->fsm, sdk_get_time_ns());
    actuator_slew_init(&ctx->slew);
    return ctx;
}

int safepulse_load_rules_json(safepulse_ctx_t *ctx, const char *config_content) {
    if (!ctx || !config_content) return -1;
    return linter_load_rules_string(&ctx->linter, &ctx->fsm.starvation_limit_ns, config_content);
}

int safepulse_load_rules_file(safepulse_ctx_t *ctx, const char *file_path) {
    if (!ctx || !file_path) return -1;
    return linter_load_rules_file(&ctx->linter, &ctx->fsm.starvation_limit_ns, file_path);
}

int safepulse_seal_packet(safepulse_packet_t *pkt, uint8_t alive_counter) {
    if (!pkt) return -1;

    pkt->magic = SAFEPULSE_MAGIC;
    pkt->safety_score = 100.0f;
    pkt->violation_code = VIOLATION_NONE;
    autosar_e2e_stamp_packet(pkt, alive_counter);
    pkt->crc32 = safepulse_crc32((const uint8_t *)pkt, offsetof(safepulse_packet_t, crc32));
    return 0;
}

static const char *get_violation_str(uint32_t code) {
    switch (code) {
        case VIOLATION_NONE: return "NONE";
        case VIOLATION_OUT_OF_BOUNDS: return "PHYSICAL_OUT_OF_BOUNDS (0x1001)";
        case VIOLATION_JERK_EXCEEDED: return "JERK_EXCEEDED (0x1002)";
        case VIOLATION_DEADLINE_MISS: return "DEADLINE_MISS (0x1003)";
        case VIOLATION_CRC_CORRUPTED: return "CRC_CORRUPTED (0x1004)";
        case VIOLATION_PLAUSIBILITY: return "PLAUSIBILITY_CONTRADICTION (0x1005)";
        case VIOLATION_E2E_REPEATED: return "E2E_ALIVE_REPEATED (0x1006)";
        case VIOLATION_E2E_SEQ_LOSS: return "E2E_ALIVE_SEQ_LOSS (0x1007)";
        case VIOLATION_STARVATION: return "STARVATION_WATCHDOG (0x1008)";
        case VIOLATION_CRASH_SIGSEGV: return "CRASH_SIGSEGV (0x1009)";
        case VIOLATION_CRASH_SIGFPE: return "CRASH_SIGFPE (0x100A)";
        case VIOLATION_CRASH_SIGABRT: return "CRASH_SIGABRT (0x100B)";
        case VIOLATION_CRASH_SIGILL: return "CRASH_SIGILL (0x100C)";
        default: return "UNKNOWN_VIOLATION";
    }
}

int safepulse_inspect(safepulse_ctx_t *ctx, safepulse_packet_t *pkt, safepulse_verdict_t *verdict) {
    if (!ctx || !pkt || !verdict) return -1;

    /* 1. Linter inspection (modifies pkt->safety_score, pkt->violation_code, pkt->status_flags in-place) */
    linter_inspect_packet(&ctx->linter, pkt);

    /* 2. Step Safe State FSM and track per-channel starvation heartbeat */
    asil_safe_state_t st = safe_state_fsm_step(&ctx->fsm, pkt->violation_code, pkt->safety_score, pkt->timestamp_ns);
    safe_state_fsm_record_channel_rx(&ctx->fsm, pkt->sensor_type, pkt->timestamp_ns);

    /* 3. Output verdict */
    verdict->passed = (pkt->violation_code == VIOLATION_NONE);
    verdict->violation_code = pkt->violation_code;
    verdict->safety_score = pkt->safety_score;
    verdict->fsm_state = (safepulse_fsm_state_t)st;
    verdict->violation_name = get_violation_str(pkt->violation_code);
    verdict->starved_sensor = ctx->fsm.starved_sensor_type;

    return 0;
}

safepulse_fsm_state_t safepulse_check_starvation(safepulse_ctx_t *ctx, uint64_t current_time_ns) {
    if (!ctx) return SAFEPULSE_FSM_EMERGENCY_STOP;
    safe_state_fsm_check_starvation(&ctx->fsm, current_time_ns);
    return (safepulse_fsm_state_t)ctx->fsm.current_state;
}

safepulse_fsm_state_t safepulse_get_fsm_state(safepulse_ctx_t *ctx) {
    if (!ctx) return SAFEPULSE_FSM_EMERGENCY_STOP;
    return (safepulse_fsm_state_t)ctx->fsm.current_state;
}

uint8_t safepulse_get_starved_sensor(const safepulse_ctx_t *ctx) {
    if (!ctx) return 0U;
    return ctx->fsm.starved_sensor_type;
}

int safepulse_apply_actuator_limits(safepulse_ctx_t *ctx, safepulse_packet_t *pkt) {
    if (!ctx || !pkt) return -EINVAL;

    asil_safe_state_t cur_state = ctx->fsm.current_state;
    actuator_limits_t lim = safe_state_get_limits(cur_state);
    /* Use packet timestamp if provided, fallback to monotonic clock domain if 0 */
    uint64_t effective_ts_ns = (pkt->timestamp_ns > 0ULL) ? pkt->timestamp_ns : sdk_get_time_ns();

    if (pkt->sensor_type == SENSOR_TYPE_STEERING) {
        double prev_steer = ctx->slew.last_clamped_steering;
        uint64_t prev_steer_ts = ctx->slew.last_steer_timestamp_ns;
        pkt->val_primary = actuator_slew_clamp_steering(
            &ctx->slew, pkt->val_primary, lim.max_steering_angle, lim.max_steering_slew_deg_s, effective_ts_ns);
        if (cur_state == ASIL_STATE_EMERGENCY_STOP) {
            double rate = 0.0;
            if (prev_steer_ts != 0 && effective_ts_ns > prev_steer_ts) {
                rate = (pkt->val_primary - prev_steer) /
                       ((double)(effective_ts_ns - prev_steer_ts) / 1000000000.0);
            }
            pkt->val_secondary = rate;
        }
        if (pkt->val_secondary > lim.max_steering_slew_deg_s) {
            pkt->val_secondary = lim.max_steering_slew_deg_s;
        } else if (pkt->val_secondary < -lim.max_steering_slew_deg_s) {
            pkt->val_secondary = -lim.max_steering_slew_deg_s;
        }
    } else if (pkt->sensor_type == SENSOR_TYPE_THROTTLE) {
        pkt->val_primary = actuator_slew_clamp_throttle(
            &ctx->slew, pkt->val_primary, lim.max_throttle_pct, effective_ts_ns);
        if (cur_state != ASIL_STATE_EMERGENCY_STOP) {
            pkt->val_secondary = actuator_slew_clamp_speed(
                &ctx->slew, pkt->val_secondary, lim.max_speed_kmh, effective_ts_ns);
        } else {
            pkt->val_secondary = actuator_brake_speed_to_stop(
                &ctx->slew, ESTOP_BRAKE_DECEL_KMH_S, pkt->val_secondary, effective_ts_ns);
        }
    } else if (pkt->sensor_type == SENSOR_TYPE_BRAKE) {
        if (!isfinite(pkt->val_primary) || pkt->val_primary < lim.min_brake_bar) {
            pkt->val_primary = lim.min_brake_bar;
        }
    }

    pkt->reserved[8]  = (uint8_t)cur_state;
    pkt->reserved[11] = SAFEPULSE_STATE_MARKER;
    pkt->crc32 = safepulse_crc32((const uint8_t *)pkt, offsetof(safepulse_packet_t, crc32));
    return 0;
}

static void sdk_reset_fsm_and_slew(safepulse_ctx_t *ctx) {
    uint64_t saved_starv_ns = ctx->fsm.starvation_limit_ns;
    safe_state_fsm_init(&ctx->fsm, sdk_get_time_ns());
    if (saved_starv_ns > 0ULL) {
        ctx->fsm.starvation_limit_ns = saved_starv_ns;
    }
    actuator_slew_state_t prev = ctx->slew;
    actuator_slew_init(&ctx->slew);
    ctx->slew.last_steer_timestamp_ns = prev.last_steer_timestamp_ns;
    ctx->slew.last_throttle_timestamp_ns = prev.last_throttle_timestamp_ns;
    ctx->slew.last_brake_timestamp_ns = prev.last_brake_timestamp_ns;
    ctx->slew.last_speed_timestamp_ns = prev.last_speed_timestamp_ns;
}

int safepulse_try_reset_fsm(safepulse_ctx_t *ctx) {
    if (!ctx) return -EINVAL;
    if (ctx->fsm.current_state == ASIL_STATE_EMERGENCY_STOP) {
        if (ctx->slew.last_speed_timestamp_ns == 0ULL) {
            /* Fail-safe: No speed measurement ever received, refuse reset */
            return -EAGAIN;
        }
        if (ctx->slew.last_clamped_speed >= 0.05) {
            /* Interlock: Must be fully stopped (< 0.05 km/h) before operator reset */
            return -EBUSY;
        }
    }
    sdk_reset_fsm_and_slew(ctx);
    return 0;
}

void safepulse_reset_fsm(safepulse_ctx_t *ctx) {
    /* Enforce strict speed interlock on public reset API as well */
    (void)safepulse_try_reset_fsm(ctx);
}

void safepulse_destroy(safepulse_ctx_t *ctx) {
    if (ctx) free(ctx);
}
