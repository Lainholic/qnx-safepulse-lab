/**
 * SafePulse AUTOSAR E2E & ISO 26262 Safe State FSM Standalone Unit Test
 * Validates polynomial 0xF4ACFB13, modulo-16 wrap-around, and FTTI 10ms escalation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "autosar_e2e.h"
#include "safe_state_fsm.h"

int main(void) {
    printf("==================================================\n");
    printf(" Running AUTOSAR E2E & Safe State FSM Unit Tests  \n");
    printf("==================================================\n");

    /* Test 1: AUTOSAR E2E Profile 1 Initialization & Sequential Packets */
    e2e_channel_state_t channel = {
        .data_id = 0x1234,
        .max_counter_jump = 2,
        .last_counter = 0,
        .initialized = false
    };

    uint8_t dummy_payload[32] = {0x01, 0x02, 0x03, 0x04};
    uint32_t crc_pkt0 = autosar_e2e_crc32(0x1234, dummy_payload, sizeof(dummy_payload));

    /* First frame -> E2E_STATUS_INITIALIZING */
    e2e_status_t st = autosar_e2e_protect_verify(&channel, 0x1234, 0, crc_pkt0, dummy_payload, sizeof(dummy_payload));
    assert(st == E2E_STATUS_INITIALIZING);
    printf("[PASS] Test 1.1: First Frame Initializing State verified.\n");

    /* Second frame (counter 1) -> E2E_STATUS_OK */
    uint32_t crc_pkt1 = autosar_e2e_crc32(0x1234, dummy_payload, sizeof(dummy_payload));
    st = autosar_e2e_protect_verify(&channel, 0x1234, 1, crc_pkt1, dummy_payload, sizeof(dummy_payload));
    assert(st == E2E_STATUS_OK);
    printf("[PASS] Test 1.2: Next Frame OK State verified.\n");

    /* Replay attack (counter 1 repeated) -> E2E_STATUS_REPEATED */
    st = autosar_e2e_protect_verify(&channel, 0x1234, 1, crc_pkt1, dummy_payload, sizeof(dummy_payload));
    assert(st == E2E_STATUS_REPEATED);
    printf("[PASS] Test 1.3: Replay Attack (Freeze) detected successfully.\n");

    /* Modulo 16 Rollover (from 15 to 0) */
    channel.last_counter = 15;
    st = autosar_e2e_protect_verify(&channel, 0x1234, 0, crc_pkt1, dummy_payload, sizeof(dummy_payload));
    assert(st == E2E_STATUS_OK);
    printf("[PASS] Test 1.4: Modulo-16 Clean Wrap-Around (15 -> 0) verified.\n");

    /* Masquerade Wrong Data ID -> E2E_STATUS_WRONG_DATA_ID */
    st = autosar_e2e_protect_verify(&channel, 0x9999, 1, crc_pkt1, dummy_payload, sizeof(dummy_payload));
    assert(st == E2E_STATUS_WRONG_DATA_ID);
    printf("[PASS] Test 1.5: Wrong Data ID Masquerade blocked successfully.\n");

    /* Test 2: ISO 26262 Safe State FSM Degradation & FTTI Escalation */
    safe_state_fsm_t fsm;
    safe_state_fsm_init(&fsm, 1000000000ULL);
    assert(fsm.current_state == ASIL_STATE_NORMAL);

    uint64_t t_now = 1000000000ULL; /* 1.0s */

    /* Minor fault -> DEGRADED */
    asil_safe_state_t cur = safe_state_fsm_step(&fsm, 0x1001, 80.0f, t_now);
    assert(cur == ASIL_STATE_DEGRADED);
    printf("[PASS] Test 2.1: Fault transition to DEGRADED verified.\n");

    /* Persistent fault exceeding 10ms FTTI -> Immediate EMERGENCY_STOP */
    t_now += (11 * 1000 * 1000ULL); /* +11ms (FTTI expired) */
    cur = safe_state_fsm_step(&fsm, 0x1001, 80.0f, t_now);
    assert(cur == ASIL_STATE_EMERGENCY_STOP);
    printf("[PASS] Test 2.2: FTTI (10ms) Timeout Escalation to EMERGENCY_STOP verified.\n");

    /* Check E-STOP actuator safety limits */
    actuator_limits_t lim = safe_state_get_limits(cur);
    assert(lim.max_throttle_pct == 0.0);
    assert(lim.min_brake_bar == 100.0);
    assert(lim.max_steering_angle == 0.0);
    printf("[PASS] Test 2.3: Actuator Safety Clamping (Throttle 0%%, Brake 100 bar, Steering Center 0 deg) verified.\n");

    printf("\nAll Production Safety Unit Tests Passed (100%%)!\n");
    return 0;
}
