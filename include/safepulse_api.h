/**
 * SafePulse Embedded Safety SDK - Public C API
 * Unified Header for Linter, AUTOSAR E2E Profile 1, and Safe State FSM Watchdog
 *
 * Conforms to ISO 26262 ASIL-D Embedded Safety Rules
 */

#ifndef SAFEPULSE_API_H
#define SAFEPULSE_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "safepulse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Safe State FSM Modes */
typedef enum {
    SAFEPULSE_FSM_NORMAL         = 0,
    SAFEPULSE_FSM_DEGRADED       = 1,
    SAFEPULSE_FSM_LIMP_HOME      = 2,
    SAFEPULSE_FSM_EMERGENCY_STOP = 3
} safepulse_fsm_state_t;

/* Inspection Result Container */
typedef struct {
    bool                  passed;
    uint32_t              violation_code;
    float                 safety_score;
    safepulse_fsm_state_t fsm_state;
    const char           *violation_name;
} safepulse_verdict_t;

/* SDK Context Handle */
typedef struct safepulse_ctx safepulse_ctx_t;

/**
 * Initialize SafePulse Runtime Engine
 * @return Allocated context handle, or NULL on failure
 */
safepulse_ctx_t *safepulse_init(void);

/**
 * Load Dynamic Verification Rules from JSON/YAML String
 * @param ctx Context handle
 * @param config_content String content of JSON configuration
 * @return 0 on success, negative error code on failure
 */
int safepulse_load_rules_json(safepulse_ctx_t *ctx, const char *config_content);

/**
 * Load Rules from File Path (.json or .yaml)
 */
int safepulse_load_rules_file(safepulse_ctx_t *ctx, const char *file_path);

/**
 * Inspect Incoming Telemetry Packet against Linter & AUTOSAR E2E Rules
 * @param ctx Context handle
 * @param pkt Pointer to 64-byte packet (modified in-place with score, violation, and kernel metadata)
 * @param verdict Output verdict details
 * @return 0 on success, negative error code on invalid parameters
 */
int safepulse_inspect(safepulse_ctx_t *ctx, safepulse_packet_t *pkt, safepulse_verdict_t *verdict);

/**
 * Check Starvation Watchdog Timer
 * @param ctx Context handle
 * @param current_time_ns Current monotonic clock in nanoseconds
 * @return Current FSM state
 */
safepulse_fsm_state_t safepulse_check_starvation(safepulse_ctx_t *ctx, uint64_t current_time_ns);

/**
 * Get Current Safe State FSM
 */
safepulse_fsm_state_t safepulse_get_fsm_state(safepulse_ctx_t *ctx);

/**
 * Reset FSM back to NORMAL
 */
void safepulse_reset_fsm(safepulse_ctx_t *ctx);

/**
 * Destroy Context and Free Resources
 */
void safepulse_destroy(safepulse_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_API_H */
