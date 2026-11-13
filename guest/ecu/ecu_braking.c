/**
 * SafePulse Virtual ECU - Electronic Brake Control Module (EBCM) - ASIL-D Hardened
 * Emits hydraulic brake pressure (0 to 120 bar) at 100Hz (10ms deadline).
 * Priority: 22 (High SCHED_FIFO real-time priority).
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
#define ECU_PRIORITY        22
#define CYCLE_PERIOD_NS     (10 * 1000 * 1000ULL)  /* 10ms */
#define DEADLINE_MARGIN_NS  (500 * 1000ULL)        /* 0.5ms safety margin */

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

    struct sched_param sp;
    sp.sched_priority = ECU_PRIORITY;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        perror("pthread_setschedparam (warning: running with default priority)");
    }

    printf("[ECU-Braking] ASIL-D Hardened. Priority: %d, Cycle: 10ms, PID: %d\n", ECU_PRIORITY, getpid());

    int fd = -1;
    uint32_t seq = 0;
    double pressure = 0.0;

    uint64_t base_time = get_time_ns();
    uint64_t cycle_count = 0;

    while (g_running) {
        cycle_count++;
        uint64_t cycle_start = get_time_ns();

        if (fd < 0) {
            fd = open(SENSOR_DEV_PATH, O_WRONLY | O_NONBLOCK);
            if (fd < 0) {
                usleep(5000);
            }
        }

        safepulse_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.seq_num = ++seq;
        pkt.timestamp_ns = cycle_start;
        pkt.task_id = (uint32_t)getpid();
        pkt.sensor_type = SENSOR_TYPE_BRAKE;
        pkt.status_flags = STATUS_FLAG_NORMAL;
        pkt.violation_code = VIOLATION_NONE;
        pkt.safety_score = 100.0f;

        /* Nominal gentle braking: continuous-time 5 to 45 bar (max rate 40 bar/s) */
        double t_sec = (double)(cycle_start - base_time) / 1000000000.0;
        pressure = 25.0 + 20.0 * sin(t_sec * 2.0);
        if (pressure < 0.0) pressure = 0.0;
        if (pressure > 150.0) {
            pkt.status_flags |= STATUS_FLAG_CRITICAL;
            pkt.violation_code = VIOLATION_OUT_OF_BOUNDS;
            pkt.safety_score = 0.0f;
            pressure = 0.0;
        }

        pkt.val_primary = pressure;
        pkt.val_secondary = -1.5 * (pressure / 45.0);

        uint64_t comp_ns = get_time_ns() - cycle_start;
        if (comp_ns > (CYCLE_PERIOD_NS - DEADLINE_MARGIN_NS)) {
            pkt.status_flags |= STATUS_FLAG_WARNING;
            pkt.violation_code = VIOLATION_DEADLINE_MISS;
            pkt.safety_score = 60.0f;
        }

        autosar_e2e_stamp_packet(&pkt, (uint8_t)(seq & 0x0F));
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

        /* After a stall (e.g. SIGSTOP during real CAN), skip missed cycles
           instead of emitting a catch-up burst. */
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
    printf("[ECU-Braking] Exited cleanly.\n");
    return 0;
}
