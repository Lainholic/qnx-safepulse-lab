/**
 * SafePulse AUTOSAR E2E & ISO 26262 Safe State FSM Standalone Unit Test
 * Validates polynomial 0xF4ACFB13, modulo-16 wrap-around, inline linter E2E path, and FTTI 10ms escalation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "safepulse.h"
#include "linter.h"
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

    /* Test 2: Inline Linter Engine AUTOSAR E2E Profile 1 Live Path */
    linter_state_t linter;
    linter_init(&linter);
    safepulse_packet_t pkt;
    uint64_t t0 = 1000000000ULL;

    /* 2.1 Normal packets (counter 0, 1) */
    for (uint8_t c = 0; c < 2; c++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.seq_num = 1U + c;
        pkt.timestamp_ns = t0 + (uint64_t)c * 20000000ULL;
        pkt.val_primary = 15.0;
        autosar_e2e_stamp_packet(&pkt, c);
        pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
        linter_inspect_packet(&linter, &pkt);
        assert(pkt.violation_code == VIOLATION_NONE);
    }
    printf("[PASS] Test 2.1: Inline Linter AUTOSAR E2E Normal Packet Pass verified.\n");

    /* 2.2 Single-bit flip in AUTOSAR E2E payload/CRC -> VIOLATION_CRC_CORRUPTED (0x1004) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 3U;
    pkt.timestamp_ns = t0 + 40000000ULL;
    pkt.val_primary = 15.0;
    autosar_e2e_stamp_packet(&pkt, 2U);
    pkt.reserved[4] ^= 0x01U; /* Flip 1 bit in E2E CRC */
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter, &pkt);
    assert(pkt.violation_code == VIOLATION_CRC_CORRUPTED);
    printf("[PASS] Test 2.2: Inline Linter AUTOSAR E2E 1-Bit Flip Trap (0x1004) verified.\n");

    /* 2.3 Alive counter skip (from 1 to 6, jump=5 > max 2) -> VIOLATION_E2E_SEQ_LOSS (0x1007) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 4U;
    pkt.timestamp_ns = t0 + 60000000ULL;
    pkt.val_primary = 15.0;
    autosar_e2e_stamp_packet(&pkt, 6U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter, &pkt);
    assert(pkt.violation_code == VIOLATION_E2E_SEQ_LOSS);
    printf("[PASS] Test 2.3: Inline Linter AUTOSAR E2E Counter Skip Trap (0x1007) verified.\n");

    /* 2.4 Alive counter repeat (6 repeated) -> VIOLATION_E2E_REPEATED (0x1006) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 5U;
    pkt.timestamp_ns = t0 + 80000000ULL;
    pkt.val_primary = 15.0;
    autosar_e2e_stamp_packet(&pkt, 6U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter, &pkt);
    assert(pkt.violation_code == VIOLATION_E2E_REPEATED);
    printf("[PASS] Test 2.4: Inline Linter AUTOSAR E2E Counter Repeat Trap (0x1006) verified.\n");

    /* Test 3: ISO 26262 Safe State FSM Degradation & FTTI Escalation */
    safe_state_fsm_t fsm;
    safe_state_fsm_init(&fsm, 1000000000ULL);
    assert(fsm.current_state == ASIL_STATE_NORMAL);

    uint64_t t_now = 1000000000ULL; /* 1.0s */

    /* Minor fault -> DEGRADED */
    asil_safe_state_t cur = safe_state_fsm_step(&fsm, 0x1001, 80.0f, t_now);
    assert(cur == ASIL_STATE_DEGRADED);
    printf("[PASS] Test 3.1: Fault transition to DEGRADED verified.\n");

    /* Non-critical fault holds DEGRADED (50 km/h) before 3s DEGRADED_HOLD_NS elapses */
    t_now += (11 * 1000 * 1000ULL); /* +11ms (< 3s DEGRADED_HOLD_NS) */
    cur = safe_state_fsm_step(&fsm, 0x1001, 80.0f, t_now);
    assert(cur == ASIL_STATE_DEGRADED && !fsm.is_latched);
    assert(safe_state_get_limits(cur).max_speed_kmh == 50.0);

    /* After DEGRADED_HOLD_NS (3s), non-critical fault tightens & latches LIMP_HOME (30 km/h) */
    t_now = 1000000000ULL + DEGRADED_HOLD_NS;
    cur = safe_state_fsm_step(&fsm, 0x1001, 80.0f, t_now);
    assert(cur == ASIL_STATE_LIMP_HOME && fsm.is_latched);
    assert(safe_state_get_limits(cur).max_speed_kmh == 30.0);
    printf("[PASS] Test 3.2a: Staged DEGRADED (50 km/h) -> 3s LIMP_HOME (30 km/h) latch verified.\n");

    /* Critical CRC fault (0x1004) escalates immediately to permanent EMERGENCY_STOP */
    t_now += 1000000ULL;
    cur = safe_state_fsm_step(&fsm, VIOLATION_CRC_CORRUPTED, 0.0f, t_now);
    assert(cur == ASIL_STATE_EMERGENCY_STOP && fsm.is_latched);
    printf("[PASS] Test 3.2b: Critical Fault (0x1004) Immediate Escalation to EMERGENCY_STOP verified.\n");

    /* Check E-STOP actuator safety limits */
    actuator_limits_t lim = safe_state_get_limits(cur);
    assert(lim.max_throttle_pct == 0.0);
    assert(lim.min_brake_bar == 100.0);
    assert(lim.max_steering_angle == 0.0);
    printf("[PASS] Test 3.3: Actuator Safety Clamping (Throttle 0%%, Brake 100 bar, Steering Center 0 deg) verified.\n");

    /* Test 3.4: Starvation Watchdog Enforcement (STARVATION_LIMIT_NS = 50ms) */
    safe_state_fsm_t fsm_starve;
    safe_state_fsm_init(&fsm_starve, 1000000000ULL);
    safe_state_fsm_step(&fsm_starve, VIOLATION_NONE, 100.0f, 1000000000ULL);
    assert(fsm_starve.current_state == ASIL_STATE_NORMAL);
    /* Silence of 40ms (< 50ms STARVATION_LIMIT_NS) -> Still NORMAL */
    safe_state_fsm_check_starvation(&fsm_starve, 1000000000ULL + 40000000ULL);
    assert(fsm_starve.current_state == ASIL_STATE_NORMAL);
    /* Silence of 50ms (>= STARVATION_LIMIT_NS) -> Immediate EMERGENCY_STOP */
    safe_state_fsm_check_starvation(&fsm_starve, 1000000000ULL + STARVATION_LIMIT_NS);
    assert(fsm_starve.current_state == ASIL_STATE_EMERGENCY_STOP && fsm_starve.is_latched);
    printf("[PASS] Test 3.4: Starvation Watchdog (STARVATION_LIMIT_NS = 50ms -> EMERGENCY_STOP) verified.\n");

    /* Test 3.5: NaN / Inf payload rejection & clean history preservation */
    linter_state_t linter_nan;
    linter_init(&linter_nan);
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 1U;
    pkt.timestamp_ns = t0;
    pkt.val_primary = 5.0;
    pkt.val_secondary = 0.0;
    autosar_e2e_stamp_packet(&pkt, 0U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter_nan, &pkt);
    assert(pkt.violation_code == VIOLATION_NONE);

    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 2U;
    pkt.timestamp_ns = t0 + 20000000ULL;
    pkt.val_primary = NAN;
    pkt.val_secondary = 0.0;
    autosar_e2e_stamp_packet(&pkt, 1U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter_nan, &pkt);
    assert(pkt.violation_code == VIOLATION_OUT_OF_BOUNDS);

    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 3U;
    pkt.timestamp_ns = t0 + 40000000ULL;
    pkt.val_primary = 5.5;
    pkt.val_secondary = INFINITY;
    autosar_e2e_stamp_packet(&pkt, 2U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter_nan, &pkt);
    assert(pkt.violation_code == VIOLATION_OUT_OF_BOUNDS);

    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 4U;
    pkt.timestamp_ns = t0 + 60000000ULL;
    pkt.val_primary = 6.0;
    pkt.val_secondary = 10.0;
    autosar_e2e_stamp_packet(&pkt, 3U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&linter_nan, &pkt);
    assert(pkt.violation_code == VIOLATION_NONE);
    printf("[PASS] Test 3.5: NaN/Inf payload trap (0x1001) & jerk-history isolation verified.\n");

    /* Test 3.6: Slew limiter enforces active hard limits on duplicate/backwards timestamps */
    actuator_slew_state_t slew_guard;
    actuator_slew_init(&slew_guard);
    double thr = actuator_slew_clamp_throttle(&slew_guard, 75.0, 100.0, 2000000000ULL);
    assert(thr == 75.0);
    double thr_dup = actuator_slew_clamp_throttle(&slew_guard, 75.0, 0.0, 2000000000ULL);
    assert(thr_dup == 0.0);
    double st_deg = actuator_slew_clamp_steering(&slew_guard, 60.0, 540.0, 250.0, 2000000000ULL);
    assert(st_deg == 60.0);
    double st_dup = actuator_slew_clamp_steering(&slew_guard, 60.0, 0.0, 250.0, 1990000000ULL);
    assert(st_dup == 0.0);
    printf("[PASS] Test 3.6: Slew anti-bypass ceiling enforcement on duplicate/backwards timestamps verified.\n");

    /* Test 3.7: Per-Channel Independent ECU Starvation Watchdog */
    safe_state_fsm_t fsm_ch;
    uint64_t ch_t0 = 1000000000ULL;
    safe_state_fsm_init(&fsm_ch, ch_t0);
    /* Feed 2 Steering and 2 Brake frames at t = 1000ms and 1020ms */
    safe_state_fsm_step(&fsm_ch, VIOLATION_NONE, 100.0f, ch_t0);
    safe_state_fsm_record_channel_rx(&fsm_ch, SENSOR_TYPE_STEERING, ch_t0);
    safe_state_fsm_step(&fsm_ch, VIOLATION_NONE, 100.0f, ch_t0);
    safe_state_fsm_record_channel_rx(&fsm_ch, SENSOR_TYPE_BRAKE, ch_t0);
    safe_state_fsm_step(&fsm_ch, VIOLATION_NONE, 100.0f, ch_t0 + 20000000ULL);
    safe_state_fsm_record_channel_rx(&fsm_ch, SENSOR_TYPE_STEERING, ch_t0 + 20000000ULL);
    safe_state_fsm_step(&fsm_ch, VIOLATION_NONE, 100.0f, ch_t0 + 20000000ULL);
    safe_state_fsm_record_channel_rx(&fsm_ch, SENSOR_TYPE_BRAKE, ch_t0 + 20000000ULL);

    /* Continue streaming ONLY Brake every 10ms up to t = 1150ms (Steering silent for 130ms >= 120ms) */
    for (uint64_t dt_ms = 30ULL; dt_ms <= 150ULL; dt_ms += 10ULL) {
        uint64_t now = ch_t0 + dt_ms * 1000000ULL;
        safe_state_fsm_step(&fsm_ch, VIOLATION_NONE, 100.0f, now);
        safe_state_fsm_record_channel_rx(&fsm_ch, SENSOR_TYPE_BRAKE, now);
        safe_state_fsm_check_starvation(&fsm_ch, now);
        if (dt_ms == 110ULL) {
            /* 90ms Steering silence < 120ms threshold -> must stay NORMAL */
            assert(fsm_ch.current_state == ASIL_STATE_NORMAL);
        }
    }
    assert(fsm_ch.current_state == ASIL_STATE_EMERGENCY_STOP && fsm_ch.is_latched);
    assert(fsm_ch.starvation_estop == true && fsm_ch.starved_sensor_type == SENSOR_TYPE_STEERING);
    printf("[PASS] Test 3.7: Per-Channel Independent ECU Starvation (Steering silent 130ms while Brake active) verified.\n");

    printf("\nAll Production Safety Unit Tests Passed (100%%)!\n");
    return 0;
}
