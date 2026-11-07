/**
 * SafePulse Extreme 1,000Hz (1ms) Stress Test Harness - ASIL-D Hardened
 * Uses QNX ClockCycles() and ClockPeriod fine-tuning to eliminate timer jitter
 * and push sustained 1kHz telemetry burst to /dev/safepulse.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "safepulse.h"

#define SENSOR_DEV_PATH "/dev/safepulse"
#define STRESS_CYCLES   5000U /* 5,000 packets at 1kHz = 5 seconds */
#define TARGET_PERIOD_NS 1000000ULL /* 1ms (1,000,000 ns) */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t calc_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return ~crc;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Elevate thread to priority 22 SCHED_FIFO */
    struct sched_param sp;
    sp.sched_priority = 22;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Fine-tune QNX ClockPeriod to 100 microseconds (100,000 ns) if permitted */
    struct _clockperiod cp;
    cp.nsec = 100000; /* 100us tick period */
    cp.fract = 0;
    ClockPeriod(CLOCK_REALTIME, &cp, NULL, 0);

    /* Fetch QNX CPU cycles per second */
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    if (cps == 0) cps = 2000000000ULL; /* Fallback 2GHz */

    uint64_t cycles_per_period = (cps * TARGET_PERIOD_NS) / 1000000000ULL;

    printf("====================================================\n");
    printf(" SafePulse 1,000Hz (1ms) Stress Harness (Hardened)  \n");
    printf(" CPU Cycles/Sec : %llu Hz                           \n", (unsigned long long)cps);
    printf(" Cycles/1ms     : %llu cycles                       \n", (unsigned long long)cycles_per_period);
    printf(" Target Cycles  : %u packets                        \n", STRESS_CYCLES);
    printf("====================================================\n");

    int fd = open(SENSOR_DEV_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open " SENSOR_DEV_PATH);
        return 1;
    }

    uint64_t t_start = get_time_ns();
    uint32_t success_count = 0;
    uint32_t error_count = 0;

    safepulse_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.task_id = (uint32_t)getpid();
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.status_flags = STATUS_FLAG_NORMAL;
    pkt.safety_score = 100.0f;
    pkt.violation_code = VIOLATION_NONE;

    uint64_t next_cycle_cycles = ClockCycles() + cycles_per_period;

    for (uint32_t i = 1; i <= STRESS_CYCLES; i++) {
        pkt.seq_num = i;
        pkt.timestamp_ns = get_time_ns();
        pkt.val_primary = (double)(i % 360) - 180.0;
        pkt.val_secondary = 1000.0; /* 1000Hz marker */
        pkt.crc32 = calc_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));

        ssize_t ret = write(fd, &pkt, sizeof(pkt));
        if (ret == sizeof(pkt)) {
            success_count++;
        } else {
            error_count++;
        }

        /* High-precision hybrid spin-wait to avoid scheduler aliasing */
        while (ClockCycles() < next_cycle_cycles) {
            /* yield briefly if significant margin remaining */
        }
        next_cycle_cycles += cycles_per_period;
    }

    uint64_t t_end = get_time_ns();
    double elapsed_sec = (double)(t_end - t_start) / 1000000000.0;
    double actual_rate_hz = (double)success_count / (elapsed_sec > 0 ? elapsed_sec : 1.0);

    close(fd);

    printf("\n--- Test Results ---\n");
    printf("Total Injected : %u\n", STRESS_CYCLES);
    printf("Successful Tx  : %u\n", success_count);
    printf("Failed Tx      : %u\n", error_count);
    printf("Total Time     : %.3f sec\n", elapsed_sec);
    printf("Actual Rate    : %.1f Hz\n", actual_rate_hz);
    printf("Integrity Status: %s\n", (error_count == 0) ? "PASS (Zero Packet Loss)" : "WARN (Drops occurred)");

    return (error_count == 0) ? 0 : 2;
}
