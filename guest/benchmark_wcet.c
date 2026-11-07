/**
 * SafePulse Production WCET (Worst-Case Execution Time) Profiler
 * Measures nanosecond-precision execution latency of the full inline safety pipeline:
 * [AUTOSAR E2E Profile 1 + ISO 26262 5-Rule Linter + Dual-Rail Safe State FSM]
 * Executed across 100,000 iterations using QNX ClockCycles() hardware timer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "safepulse.h"
#include "linter.h"
#include "autosar_e2e.h"
#include "safe_state_fsm.h"

#define BENCH_ITERATIONS 100000U /* 100,000 executions */

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    if (cps == 0) cps = 2000000000ULL; /* Fallback 2.0 GHz */
    double ns_per_cycle = 1000000000.0 / (double)cps;

    printf("====================================================\n");
    printf(" SafePulse WCET & Jitter Profiler (ISO 26262 ASIL-D)\n");
    printf(" Target Iterations : %u runs                        \n", BENCH_ITERATIONS);
    printf(" Hardware Clock    : %llu Hz (%.3f ns/cycle)       \n", (unsigned long long)cps, ns_per_cycle);
    printf("====================================================\n");

    /* Allocate latency samples array */
    double *latencies_ns = (double *)malloc(BENCH_ITERATIONS * sizeof(double));
    if (!latencies_ns) {
        fprintf(stderr, "Failed to allocate benchmark memory\n");
        return 1;
    }

    /* Initialize Safety Engines */
    linter_state_t linter;
    linter_init(&linter);

    safe_state_fsm_t fsm;
    safe_state_fsm_init(&fsm);

    e2e_channel_state_t e2e_channel = {
        .data_id = 0x1001,
        .max_counter_jump = 2,
        .last_counter = 0,
        .initialized = false
    };

    safepulse_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.task_id = 100;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.status_flags = STATUS_FLAG_NORMAL;
    pkt.safety_score = 100.0f;
    pkt.violation_code = VIOLATION_NONE;

    double total_ns = 0.0;
    double min_ns = 1e9;
    double max_ns = 0.0;

    /* Warm-up cache (1,000 iterations) */
    for (int w = 0; w < 1000; w++) {
        pkt.val_primary = 20.0;
        linter_inspect_packet(&linter, &pkt);
    }

    /* Production Benchmark Run */
    for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
        pkt.seq_num = i;
        pkt.timestamp_ns = 1000000ULL * i;
        pkt.val_primary = 35.0 * sin((double)i * 0.01);
        pkt.val_secondary = 0.5;

        uint64_t c_start = ClockCycles();

        /* --- Full Inline Safety Path Execution --- */
        /* 1. Linter 5-Rule Verification */
        linter_inspect_packet(&linter, &pkt);

        /* 2. AUTOSAR E2E Profile 1 Table CRC & Sequence Verification */
        uint8_t rolling_counter = (uint8_t)(i & 0x0F);
        uint32_t e2e_crc = autosar_e2e_crc32(e2e_channel.data_id, (const uint8_t *)&pkt, 48);
        e2e_status_t e2e_st = autosar_e2e_protect_verify(&e2e_channel, e2e_channel.data_id,
                                                         rolling_counter, e2e_crc,
                                                         (const uint8_t *)&pkt, 48);
        if (e2e_st != E2E_STATUS_OK && e2e_st != E2E_STATUS_INITIALIZING) {
            pkt.violation_code = autosar_e2e_status_to_violation(e2e_st);
        }

        /* 3. Dual-Rail ISO 26262 Safe State FSM Step & Clamping */
        asil_safe_state_t cur_state = safe_state_fsm_step(&fsm, pkt.violation_code, pkt.safety_score, pkt.timestamp_ns);
        if (cur_state != ASIL_STATE_NORMAL) {
            actuator_limits_t lim = safe_state_get_limits(cur_state);
            if (pkt.val_primary > lim.max_steering_angle) pkt.val_primary = lim.max_steering_angle;
        }
        /* ----------------------------------------- */

        uint64_t c_end = ClockCycles();

        double elapsed_ns = (double)(c_end - c_start) * ns_per_cycle;
        latencies_ns[i] = elapsed_ns;
        total_ns += elapsed_ns;
        if (elapsed_ns < min_ns) min_ns = elapsed_ns;
        if (elapsed_ns > max_ns) max_ns = elapsed_ns;
    }

    /* Compute Percentiles */
    qsort(latencies_ns, BENCH_ITERATIONS, sizeof(double), compare_doubles);
    double avg_ns = total_ns / BENCH_ITERATIONS;
    double p50_ns = latencies_ns[(size_t)(BENCH_ITERATIONS * 0.50)];
    double p95_ns = latencies_ns[(size_t)(BENCH_ITERATIONS * 0.95)];
    double p99_ns = latencies_ns[(size_t)(BENCH_ITERATIONS * 0.99)];
    double p999_ns = latencies_ns[(size_t)(BENCH_ITERATIONS * 0.999)];
    double wcet_ns = max_ns;

    printf("\n====================================================\n");
    printf(" WCET Benchmark Quantitative Results (100k runs)   \n");
    printf("====================================================\n");
    printf(" Best Case (Min)    : %.3f µs (%6.1f ns)\n", min_ns / 1000.0, min_ns);
    printf(" Median (p50)       : %.3f µs (%6.1f ns)\n", p50_ns / 1000.0, p50_ns);
    printf(" Average (Mean)     : %.3f µs (%6.1f ns)\n", avg_ns / 1000.0, avg_ns);
    printf(" 95th Percentile    : %.3f µs (%6.1f ns)\n", p95_ns / 1000.0, p95_ns);
    printf(" 99th Percentile    : %.3f µs (%6.1f ns)\n", p99_ns / 1000.0, p99_ns);
    printf(" 99.9th Percentile  : %.3f µs (%6.1f ns)\n", p999_ns / 1000.0, p999_ns);
    printf(" Worst-Case (WCET)  : %.3f µs (%6.1f ns)\n", wcet_ns / 1000.0, wcet_ns);
    printf(" ASIL-D 10ms Budget : WCET consumes %.3f%% of 10ms FTTI\n", (wcet_ns / 10000000.0) * 100.0);
    printf(" Production Verdict : %s\n", (wcet_ns < 50000.0) ? "PASS (< 50µs Hard Real-Time Cap)" : "FAIL");
    printf("====================================================\n");

    free(latencies_ns);
    return (wcet_ns < 50000.0) ? 0 : 1;
}
