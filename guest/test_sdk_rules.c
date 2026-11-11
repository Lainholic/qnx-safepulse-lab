/**
 * SafePulse C SDK Verification Test: Dynamic JSON/YAML Rule Mutation & Packet Sealing
 * Proves that safepulse_load_rules_json() mutates active linter & watchdog thresholds
 * and changes inspection verdicts on identical sensor inputs.
 */

#include <stdio.h>
#include <string.h>
#include "safepulse_api.h"

static int load_repo_rules_json(safepulse_ctx_t *ctx) {
    if (safepulse_load_rules_file(ctx, "rules.json") == 0) return 0;
    if (safepulse_load_rules_file(ctx, "../rules.json") == 0) return 0;
    if (safepulse_load_rules_file(ctx, "/proc/boot/rules.json") == 0) return 0;
    return -1;
}

static int load_truck_rules_yaml(safepulse_ctx_t *ctx) {
    if (safepulse_load_rules_file(ctx, "examples/custom_rules/truck_rules.yaml") == 0) return 0;
    if (safepulse_load_rules_file(ctx, "../examples/custom_rules/truck_rules.yaml") == 0) return 0;
    if (safepulse_load_rules_file(ctx, "/proc/boot/truck_rules.yaml") == 0) return 0;
    return -1;
}

int main(void) {
    printf("==================================================\n");
    printf(" SafePulse SDK Dynamic Rules & Seal Verification  \n");
    printf("==================================================\n");

    /* ----------------------------------------------------------------------
     * Case 1: Identical 850 deg/s Steering Jerk
     *   - Default compile-time rule (max_jerk = 720 deg/s) -> FAIL (0x1002)
     *   - Loaded rules.json (max_jerk = 1000 deg/s)        -> PASS (0x0000)
     * ---------------------------------------------------------------------- */
    safepulse_ctx_t *ctx_default = safepulse_init();
    safepulse_ctx_t *ctx_json    = safepulse_init();
    if (!ctx_default || !ctx_json) {
        fprintf(stderr, "[FAIL] Context allocation failed\n");
        return 1;
    }

    if (load_repo_rules_json(ctx_json) != 0) {
        fprintf(stderr, "[FAIL] Could not load rules.json\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        return 1;
    }

    safepulse_packet_t p1_def = {0}, p2_def = {0};
    safepulse_packet_t p1_jsn = {0}, p2_jsn = {0};
    safepulse_verdict_t v_def = {0}, v_jsn = {0};

    p1_def.seq_num = 1;
    p1_def.timestamp_ns = 10000000ULL; /* 10 ms */
    p1_def.sensor_type = SAFEPULSE_SENSOR_STEERING;
    p1_def.val_primary = 0.0;
    p1_jsn = p1_def;

    safepulse_seal_packet(&p1_def, 0);
    safepulse_seal_packet(&p1_jsn, 0);
    safepulse_inspect(ctx_default, &p1_def, &v_def);
    safepulse_inspect(ctx_json,    &p1_jsn, &v_jsn);

    /* Step +8.5 deg in 10 ms => 850.0 deg/s */
    p2_def.seq_num = 2;
    p2_def.timestamp_ns = 20000000ULL; /* 20 ms */
    p2_def.sensor_type = SAFEPULSE_SENSOR_STEERING;
    p2_def.val_primary = 8.5;
    p2_jsn = p2_def;

    safepulse_seal_packet(&p2_def, 1);
    safepulse_seal_packet(&p2_jsn, 1);
    safepulse_inspect(ctx_default, &p2_def, &v_def);
    safepulse_inspect(ctx_json,    &p2_jsn, &v_jsn);

    printf("[Case 1] 850 deg/s Jerk | Default (720): %s (0x%04X) -> rules.json (1000): %s (0x%04X)\n",
           v_def.passed ? "PASS" : "FAIL", v_def.violation_code,
           v_jsn.passed ? "PASS" : "FAIL", v_jsn.violation_code);

    if (v_def.passed || v_def.violation_code != VIOLATION_JERK_EXCEEDED || !v_jsn.passed) {
        fprintf(stderr, "[FAIL] Case 1 verdict flip assertion failed\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        return 1;
    }

    /* ----------------------------------------------------------------------
     * Case 2: Live In-Place Mutation via safepulse_load_rules_json()
     *   - Under rules.json ([-540, 540] deg), 45.0 deg -> PASS
     *   - Apply strict JSON ([-30, 30] deg, starvation 25ms) -> 45.0 deg -> FAIL (0x1001)
     * ---------------------------------------------------------------------- */
    safepulse_ctx_t *ctx_bounds = safepulse_init();
    if (!ctx_bounds || load_repo_rules_json(ctx_bounds) != 0) {
        fprintf(stderr, "[FAIL] Could not initialize Case 2 context\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        safepulse_destroy(ctx_bounds);
        return 1;
    }

    safepulse_packet_t p3 = {0};
    p3.seq_num = 1;
    p3.timestamp_ns = 20000000ULL; /* 20 ms */
    p3.sensor_type = SAFEPULSE_SENSOR_STEERING;
    p3.val_primary = 45.0;
    safepulse_seal_packet(&p3, 0);

    safepulse_verdict_t v_before = {0};
    safepulse_inspect(ctx_bounds, &p3, &v_before);

    const char *strict_json =
        "{"
        "  \"watchdog\": { \"starvation_limit_ms\": 25 },"
        "  \"sensors\": ["
        "    { \"type\": 1, \"min_val\": -30.0, \"max_val\": 30.0, \"max_jerk\": 1000.0 }"
        "  ]"
        "}";
    if (safepulse_load_rules_json(ctx_bounds, strict_json) != 0) {
        fprintf(stderr, "[FAIL] Inline safepulse_load_rules_json failed\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        safepulse_destroy(ctx_bounds);
        return 1;
    }

    safepulse_packet_t p4 = {0};
    p4.seq_num = 2;
    p4.timestamp_ns = 40000000ULL; /* 40 ms (dt=20ms <= 25ms deadline, rate=0 deg/s) */
    p4.sensor_type = SAFEPULSE_SENSOR_STEERING;
    p4.val_primary = 45.0;
    safepulse_seal_packet(&p4, 1);

    safepulse_verdict_t v_after = {0};
    safepulse_inspect(ctx_bounds, &p4, &v_after);

    printf("[Case 2] 45.0 deg Steer | Before JSON (max 540): %s (0x%04X) -> After JSON (max 30): %s (0x%04X)\n",
           v_before.passed ? "PASS" : "FAIL", v_before.violation_code,
           v_after.passed ? "PASS" : "FAIL", v_after.violation_code);

    if (!v_before.passed || v_after.passed || v_after.violation_code != VIOLATION_OUT_OF_BOUNDS) {
        fprintf(stderr, "[FAIL] Case 2 bounds mutation assertion failed\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        safepulse_destroy(ctx_bounds);
        return 1;
    }

    /* ----------------------------------------------------------------------
     * Case 3: Watchdog Starvation Threshold Mutation (50ms default -> 25ms JSON)
     * ---------------------------------------------------------------------- */
    safepulse_reset_fsm(ctx_bounds);
    safepulse_reset_fsm(ctx_default);
    safepulse_packet_t hb_jsn = {0}, hb_def = {0};
    hb_jsn.seq_num = 3;
    hb_jsn.timestamp_ns = 55000000ULL; /* 55 ms (dt=15ms <= 25ms, delta=45 deg -> use 0.0 on fresh sensor 2) */
    hb_jsn.sensor_type = SAFEPULSE_SENSOR_BRAKE;
    hb_jsn.val_primary = 10.0;
    hb_def = hb_jsn;
    safepulse_seal_packet(&hb_jsn, 0);
    safepulse_seal_packet(&hb_def, 0);
    safepulse_inspect(ctx_bounds,  &hb_jsn, &v_jsn);
    safepulse_inspect(ctx_default, &hb_def, &v_def);

    safepulse_fsm_state_t st_def_30ms = safepulse_check_starvation(ctx_default, 85000000ULL);
    safepulse_fsm_state_t st_jsn_30ms = safepulse_check_starvation(ctx_bounds,  85000000ULL);
    printf("[Case 3] 30ms Silence   | Default (50ms limit): FSM=%d -> JSON (25ms limit): FSM=%d\n",
           (int)st_def_30ms, (int)st_jsn_30ms);

    if (st_def_30ms != SAFEPULSE_FSM_NORMAL || st_jsn_30ms != SAFEPULSE_FSM_EMERGENCY_STOP) {
        fprintf(stderr, "[FAIL] Case 3 starvation threshold mutation assertion failed\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        safepulse_destroy(ctx_bounds);
        return 1;
    }

    /* ----------------------------------------------------------------------
     * Case 4: 180 bar Brake Pressure (Default 150 bar FAIL -> Truck YAML 250 bar PASS)
     * ---------------------------------------------------------------------- */
    safepulse_ctx_t *ctx_truck = safepulse_init();
    if (!ctx_truck || load_truck_rules_yaml(ctx_truck) != 0) {
        fprintf(stderr, "[FAIL] Could not load truck_rules.yaml\n");
        safepulse_destroy(ctx_default);
        safepulse_destroy(ctx_json);
        safepulse_destroy(ctx_bounds);
        safepulse_destroy(ctx_truck);
        return 1;
    }

    safepulse_packet_t brk_def = {0}, brk_trk = {0};
    brk_def.seq_num = 4;
    brk_def.timestamp_ns = 65000000ULL;
    brk_def.sensor_type = SAFEPULSE_SENSOR_BRAKE;
    brk_def.val_primary = 180.0;
    brk_trk = brk_def;
    safepulse_seal_packet(&brk_def, 1);
    safepulse_seal_packet(&brk_trk, 0);

    safepulse_inspect(ctx_default, &brk_def, &v_def);
    safepulse_inspect(ctx_truck,   &brk_trk, &v_jsn);

    printf("[Case 4] 180 bar Brake  | Default (150 bar): %s (0x%04X) -> Truck YAML (250 bar): %s (0x%04X)\n",
           v_def.passed ? "PASS" : "FAIL", v_def.violation_code,
           v_jsn.passed ? "PASS" : "FAIL", v_jsn.violation_code);

    safepulse_destroy(ctx_default);
    safepulse_destroy(ctx_json);
    safepulse_destroy(ctx_bounds);
    safepulse_destroy(ctx_truck);

    if (v_def.passed || !v_jsn.passed) {
        fprintf(stderr, "[FAIL] Case 4 brake threshold flip assertion failed\n");
        return 1;
    }

    printf("[RESULT] SDK Dynamic Rules & Seal Verification: PASS (4/4 Verdict Flips Confirmed)\n");
    return 0;
}
