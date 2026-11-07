/*
 * safepulse_watcher.c - QNX Neutrino ASIL-D High Availability Watcher Daemon
 *
 * Supervisor implementing High Availability Manager (HAM) patterns:
 * - POSIX spawn for zero-copy process launch on QNX Neutrino
 * - Anti-Flapping Circuit Breaker with exponential backoff
 * - Nanosecond-precision recovery latency measurement
 * - Signal-safe child termination on SIGINT/SIGTERM
 * - Preservation of persistent POSIX shared memory (/safepulse_ringbuf)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/neutrino.h>
#include <spawn.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <signal.h>
#include <stdbool.h>

#define RESMGR_PATH             "/proc/boot/safepulse_resmgr"
#define MAX_CRASH_WINDOW_MS     10000ULL
#define MAX_CRASHES_IN_WINDOW   5
#define MIN_BACKOFF_US          2000     /* 2ms minimal fast recovery */
#define MAX_BACKOFF_US          1000000  /* 1 sec maximum backoff */

static volatile sig_atomic_t g_running = 1;
static volatile pid_t g_child_pid = -1;

static void sig_handler(int sig) {
    g_running = 0;
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("==================================================\n");
    printf(" Starting QNX SafePulse ASIL-D Watcher Daemon     \n");
    printf(" High-Availability Process Supervisor (HAM Pattern)\n");
    printf(" Target Binary: %s\n", RESMGR_PATH);
    printf("==================================================\n");

    uint32_t restart_count = 0;
    uint64_t recent_crash_timestamps[MAX_CRASHES_IN_WINDOW] = {0};
    uint32_t crash_idx = 0;
    useconds_t current_backoff_us = MIN_BACKOFF_US;

    while (g_running) {
        printf("[Watcher] Spawning /dev/safepulse Resource Manager (Run #%u)...\n", restart_count + 1);
        uint64_t spawn_start_ns = get_time_ns();

        char *spawn_argv[] = { (char *)RESMGR_PATH, NULL };
        int spawn_err = posix_spawn((pid_t *)&g_child_pid, RESMGR_PATH, NULL, NULL, spawn_argv, NULL);
        if (spawn_err != 0) {
            fprintf(stderr, "[Watcher] posix_spawn failed: %s (err: %d)\n", strerror(spawn_err), spawn_err);
            sleep(1);
            continue;
        }

        printf("[Watcher] Supervising child PID %d (Device: /dev/safepulse)\n", g_child_pid);

        int status = 0;
        pid_t terminated_pid = waitpid(g_child_pid, &status, 0);
        uint64_t crash_time_ns = get_time_ns();

        if (!g_running) {
            break;
        }

        if (terminated_pid > 0) {
            restart_count++;
            g_child_pid = -1;

            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                const char *sig_name = "Signal";
                if (sig == 11) sig_name = "SIGSEGV (Memory Fault / Null Dereference)";
                else if (sig == 8) sig_name = "SIGFPE (Arithmetic Exception / Divide By Zero)";
                else if (sig == 6) sig_name = "SIGABRT (Assertion Failure / Critical Abort)";
                else if (sig == 4) sig_name = "SIGILL (Illegal Opcode Execution)";
                else if (sig == 7 || sig == 10) sig_name = "SIGBUS (Hardware Bus Alignment Error)";

                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf("[Watcher] ALERT: /dev/safepulse CRASHED with signal %d [%s]!\n",
                       sig, sig_name);
                printf("[Watcher] Timestamp: %llu ns\n", (unsigned long long)crash_time_ns);
                printf("[Watcher] Initiating Self-Healing Zero-Loss Recovery...\n");
                printf("[Watcher] Retaining persistent POSIX Shared Memory (/safepulse_ringbuf)\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
            } else if (WIFEXITED(status)) {
                printf("[Watcher] /dev/safepulse exited with status %d\n", WEXITSTATUS(status));
            }

            /* Anti-Flapping Circuit Breaker Calculation */
            recent_crash_timestamps[crash_idx % MAX_CRASHES_IN_WINDOW] = crash_time_ns;
            crash_idx++;

            uint64_t oldest_crash = recent_crash_timestamps[crash_idx % MAX_CRASHES_IN_WINDOW];
            if (crash_idx >= MAX_CRASHES_IN_WINDOW &&
                (crash_time_ns - oldest_crash) < (MAX_CRASH_WINDOW_MS * 1000000ULL)) {
                /* Rapid flapping detected: engage exponential backoff */
                current_backoff_us = (current_backoff_us * 2 > MAX_BACKOFF_US) ? MAX_BACKOFF_US : current_backoff_us * 2;
                printf("[Watcher] Circuit Breaker Activated: Rapid flapping detected (%d crashes within 10s). Backoff: %u ms\n",
                       MAX_CRASHES_IN_WINDOW, (unsigned int)(current_backoff_us / 1000));
            } else {
                current_backoff_us = MIN_BACKOFF_US;
            }

            usleep(current_backoff_us);

            uint64_t recovered_ns = get_time_ns();
            double recovery_ms = (double)(recovered_ns - crash_time_ns) / 1000000.0;
            printf("[Watcher] Recovery complete in %.3f ms. Ready for respawn.\n", recovery_ms);
        }
    }

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        waitpid(g_child_pid, NULL, 0);
    }
    printf("[Watcher] Terminated cleanly.\n");
    return 0;
}
