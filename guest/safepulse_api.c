/**
 * SafePulse Embedded Safety SDK Implementation
 * Encapsulates Linter Engine, AUTOSAR E2E Profile 1, and Safe State FSM Watchdog
 */

#include <stdlib.h>
#include <time.h>

#include "safepulse_api.h"
#include "safepulse.h"
#include "linter.h"
#include "safe_state_fsm.h"
#include "autosar_e2e.h"

struct safepulse_ctx {
    linter_state_t   linter;
    safe_state_fsm_t fsm;
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

void safepulse_reset_fsm(safepulse_ctx_t *ctx) {
    if (!ctx) return;
    uint64_t saved_starv_ns = ctx->fsm.starvation_limit_ns;
    safe_state_fsm_init(&ctx->fsm, sdk_get_time_ns());
    if (saved_starv_ns > 0ULL) {
        ctx->fsm.starvation_limit_ns = saved_starv_ns;
    }
}

void safepulse_destroy(safepulse_ctx_t *ctx) {
    if (ctx) free(ctx);
}
