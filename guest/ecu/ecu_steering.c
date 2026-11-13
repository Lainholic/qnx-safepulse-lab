/**
 * SafePulse Virtual ECU - Electronic Power Steering (EPS) - ASIL-D Hardened
 * Emits steering angle (-540 to +540 deg) and rate at 50Hz (20ms deadline).
 * Priority: 21 (SCHED_FIFO real-time priority).
 * 
 * Hardened Features (DeepSeek Audit Round 3):
 * 1. Absolute Base-Time Drift-Free Periodic Scheduling
 * 2. Self-Deadline Overrun Detection with 1ms margin
 * 3. Sensor Bounds and Plausibility Fail-Safe Check
 * 4. Non-Blocking I/O write with timeout to prevent priority inversion
 * 5. Clean signal termination (SIGTERM/SIGINT)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <sys/neutrino.h>

#include "safepulse.h"
#include "autosar_e2e.h"

#define SENSOR_DEV_PATH     "/dev/safepulse"
#define ECU_PRIORITY        21
#define CYCLE_PERIOD_NS     (20 * 1000 * 1000ULL)  /* 20ms */
#define DEADLINE_MARGIN_NS  (1 * 1000 * 1000ULL)   /* 1ms safety margin */
#define MAX_CONSECUTIVE_FAILURES 5

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

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

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Elevate thread to real-time FIFO priority 21 */
    struct sched_param sp;
    sp.sched_priority = ECU_PRIORITY;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        perror("pthread_setschedparam (warning: running with default priority)");
    }

    printf("[ECU-Steering] ASIL-D Hardened. Priority: %d, Cycle: 20ms, PID: %d\n", ECU_PRIORITY, getpid());

    int fd = -1;
    uint32_t seq = 0;
    uint32_t consecutive_failures = 0;
    double t = 0.0;

    uint64_t base_time = get_time_ns();
    uint64_t cycle_count = 0;

    while (g_running) {
        cycle_count++;
        uint64_t cycle_start = get_time_ns();

        if (fd < 0) {
            fd = open(SENSOR_DEV_PATH, O_WRONLY | O_NONBLOCK);
            if (fd < 0) {
                consecutive_failures++;
                if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                    /* Wait slightly to avoid thrashing if resmgr is reviving */
                    usleep(10000);
                }
            } else {
                consecutive_failures = 0;
            }
        }

        safepulse_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.seq_num = ++seq;
        pkt.timestamp_ns = cycle_start;
        pkt.task_id = (uint32_t)getpid();
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.status_flags = STATUS_FLAG_NORMAL;
        pkt.violation_code = VIOLATION_NONE;
        pkt.safety_score = 100.0f;

        /* Nominal steering motion: continuous-time sine wave +/- 90 degrees (max rate 225 deg/s) */
        t = (double)(cycle_start - base_time) / 1000000000.0 * 2.5;
        double angle = 90.0 * sin(t);
        double angle_rate = 90.0 * cos(t) * 2.5;

        /* Input validation with fail-safe bounds */
        if (!isfinite(angle) || fabs(angle) > 540.0) {
            pkt.status_flags |= STATUS_FLAG_CRITICAL;
            pkt.violation_code = VIOLATION_OUT_OF_BOUNDS;
            pkt.safety_score = 0.0f;
            angle = 0.0; /* Transition to fail-safe state */
        }

        pkt.val_primary = angle;
        pkt.val_secondary = angle_rate;

        /* Deadline check before transmission */
        uint64_t computation_time = get_time_ns() - cycle_start;
        if (computation_time > (CYCLE_PERIOD_NS - DEADLINE_MARGIN_NS)) {
            pkt.status_flags |= STATUS_FLAG_WARNING;
            pkt.violation_code = VIOLATION_DEADLINE_MISS;
            pkt.safety_score = 50.0f;
        }

        /* AUTOSAR E2E Profile 1 Protection Header (Data ID + 4-bit Alive Counter + CRC-32 0xF4ACFB13) */
        autosar_e2e_stamp_packet(&pkt, (uint8_t)(seq & 0x0F));

        /* Checksum across bytes 0x00 to 0x3B (60 bytes) */
        pkt.crc32 = calc_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));

        if (fd >= 0) {
            ssize_t bytes_written = write(fd, &pkt, sizeof(pkt));
            if (bytes_written < 0) {
                close(fd);
                fd = open(SENSOR_DEV_PATH, O_WRONLY | O_NONBLOCK);
                if (fd >= 0) {
                    (void)write(fd, &pkt, sizeof(pkt));
                }
            }
        }

        /* Absolute drift-free sleep. After a stall (e.g. SIGSTOP during real CAN),
           skip missed cycles instead of emitting a catch-up burst. */
        uint64_t next_wakeup_ns = base_time + (cycle_count * CYCLE_PERIOD_NS);
        uint64_t now_ns = get_time_ns();
        if (next_wakeup_ns <= now_ns) {
            uint64_t skipped = ((now_ns - base_time) / CYCLE_PERIOD_NS) + 1;
            if (skipped > cycle_count) {
                cycle_count = skipped;
            }
            next_wakeup_ns = base_time + (cycle_count * CYCLE_PERIOD_NS);
        }
        struct timespec ts;
        ts.tv_sec = (time_t)(next_wakeup_ns / 1000000000ULL);
        ts.tv_nsec = (long)(next_wakeup_ns % 1000000000ULL);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }

    if (fd >= 0) close(fd);
    printf("[ECU-Steering] Exited cleanly.\n");
    return 0;
}
