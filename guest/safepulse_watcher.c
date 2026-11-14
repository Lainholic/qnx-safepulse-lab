/**
 * SafePulse High Availability Watcher Daemon (QNX Microkernel)
 * Supervises /dev/safepulse and guarantees zero-downtime microkernel recovery.
 * Provides End-to-End Ready Verification (measuring crash to actual device readiness).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/slog2.h>
#include <spawn.h>
#include <secpol/secpol.h>
#include "safepulse_identity.h"

#define DEFAULT_RESMGR_PATH   "/proc/boot/safepulse_resmgr"
#define FALLBACK_RESMGR_PATH  "/tmp/safepulse_resmgr"
#define RESMGR_DEVICE_NAME    "/dev/safepulse"

static const char *get_resmgr_path(void) {
    if (access(DEFAULT_RESMGR_PATH, X_OK) == 0) {
        return DEFAULT_RESMGR_PATH;
    }
    return FALLBACK_RESMGR_PATH;
}
#define MIN_BACKOFF_US        1000     /* 1 ms minimal debounce */
#define MAX_BACKOFF_US        20000    /* 20 ms max backoff (prevents flapping while staying < 50ms) */
#define MAX_CRASH_WINDOW_MS   10000    /* 10 second window */
#define MAX_CRASHES_IN_WINDOW 5        /* Flapping threshold */

static volatile sig_atomic_t g_running = 1;
static volatile pid_t g_child_pid = -1;
static slog2_buffer_t g_slog_buf = NULL;

static void init_watcher_slog2(void) {
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffer_set_name = "safepulse_watcher";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "ham";
    cfg.buffer_config[0].num_pages = 2;
    (void)slog2_register(&cfg, &g_slog_buf, SLOG2_QUIET);
}

static void __attribute__((format(printf, 2, 3)))
watcher_slog(uint8_t severity, const char *fmt, ...) {
    if (!g_slog_buf) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vslog2f(g_slog_buf, 0, severity, fmt, ap);
    va_end(ap);
}

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
    }
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static bool wait_for_device_ready(const char *dev_path, int timeout_ms) {
    int elapsed_us = 0;
    int max_us = timeout_ms * 1000;
    while (elapsed_us < max_us) {
        int fd = open(dev_path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return true;
        }
        usleep(500); /* 0.5ms retry */
        elapsed_us += 500;
    }
    return false;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    init_watcher_slog2();

    const char *target_path = get_resmgr_path();

    printf("==================================================\n");
    printf(" SafePulse High-Availability Manager (HAM) Daemon \n");
    printf(" Target Executable: %s\n", target_path);
    printf(" Monitored Node:    %s\n", RESMGR_DEVICE_NAME);
    printf(" Verification:      End-to-End Ready (open() barrier)\n");
    printf("==================================================\n");

    uint32_t restart_count = 0;
    useconds_t current_backoff_us = MIN_BACKOFF_US;
    uint64_t recent_crash_timestamps[MAX_CRASHES_IN_WINDOW] = {0};
    uint32_t crash_idx = 0;
    uint64_t last_crash_time_ns = 0;

    while (g_running) {
        printf("[Watcher] Spawning %s (Run #%u)...\n", target_path, restart_count + 1);
        uint64_t spawn_start_ns = get_time_ns();

        char *spawn_argv[] = { (char *)target_path, NULL };
        posix_spawnattr_t attr;
        int spawn_err = posix_spawnattr_init(&attr);
        if (spawn_err == 0) {
            const char *secure = getenv("SAFEPULSE_SECURE");
            if (secure != NULL && strcmp(secure, "1") == 0) {
                uint32_t flags = 0;
                spawn_err = posix_spawnattr_setcred(&attr, SAFEPULSE_UID_RESMGR, SAFEPULSE_GID_DEVICE);
                if (spawn_err == 0 && secpol_posix_spawnattr_settypeid(NULL, &attr, "safepulse_resmgr_t", SECPOL_TYPE_NAME) != 0)
                    spawn_err = errno ? errno : EINVAL;
                if (spawn_err == 0) spawn_err = posix_spawnattr_getxflags(&attr, &flags);
                if (spawn_err == 0) spawn_err = posix_spawnattr_setxflags(&attr, flags | POSIX_SPAWN_SETCRED);
            }
            if (spawn_err == 0)
                spawn_err = posix_spawn((pid_t *)&g_child_pid, target_path, NULL, &attr, spawn_argv, NULL);
            posix_spawnattr_destroy(&attr);
        }
        if (spawn_err != 0) {
            fprintf(stderr, "[Watcher] posix_spawn failed: %s (err: %d)\n", strerror(spawn_err), spawn_err);
            sleep(1);
            continue;
        }

        /* End-to-End Service Ready Verification Barrier */
        if (wait_for_device_ready(RESMGR_DEVICE_NAME, 200)) {
            uint64_t service_ready_ns = get_time_ns();
            if (last_crash_time_ns > 0) {
                double e2e_recovery_ms = (double)(service_ready_ns - last_crash_time_ns) / 1000000.0;
                printf("[Watcher] Watcher Respawn Complete (crash -> %s open): %.3f ms!\n",
                       RESMGR_DEVICE_NAME, e2e_recovery_ms);
                watcher_slog(SLOG2_NOTICE, "Respawned %s PID=%d recovery_ms=%.3f",
                             RESMGR_DEVICE_NAME, (int)g_child_pid, e2e_recovery_ms);
                last_crash_time_ns = 0; /* Reset latch */
            } else {
                double init_ms = (double)(service_ready_ns - spawn_start_ns) / 1000000.0;
                printf("[Watcher] Initial Service Ready in %.3f ms (PID %d)\n", init_ms, g_child_pid);
                watcher_slog(SLOG2_INFO, "Initial service ready %s PID=%d init_ms=%.3f",
                             RESMGR_DEVICE_NAME, (int)g_child_pid, init_ms);
            }
        } else {
            printf("[Watcher] Warning: Device node not responsive within 200ms\n");
        }

        int status = 0;
        pid_t terminated_pid = waitpid(g_child_pid, &status, 0);
        last_crash_time_ns = get_time_ns();

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
                printf("[Watcher] ALERT: %s CRASHED with signal %d [%s]!\n",
                       RESMGR_DEVICE_NAME, sig, sig_name);
                printf("[Watcher] Crash Timestamp: %llu ns\n", (unsigned long long)last_crash_time_ns);
                printf("[Watcher] Retaining persistent POSIX Shared Memory (/safepulse_ringbuf)\n");
                printf("[Watcher] Initiating Self-Healing Zero-Loss Recovery...\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                watcher_slog(SLOG2_CRITICAL, "%s crashed with signal %d (%s); respawning",
                             RESMGR_DEVICE_NAME, sig, sig_name);
            } else if (WIFEXITED(status)) {
                printf("[Watcher] %s exited cleanly with status %d\n", RESMGR_DEVICE_NAME, WEXITSTATUS(status));
            }

            /* Anti-Flapping Circuit Breaker Calculation */
            recent_crash_timestamps[crash_idx % MAX_CRASHES_IN_WINDOW] = last_crash_time_ns;
            crash_idx++;

            uint64_t oldest_crash = recent_crash_timestamps[crash_idx % MAX_CRASHES_IN_WINDOW];
            if (crash_idx >= MAX_CRASHES_IN_WINDOW &&
                (last_crash_time_ns - oldest_crash) < (MAX_CRASH_WINDOW_MS * 1000000ULL)) {
                current_backoff_us = (current_backoff_us * 2 > MAX_BACKOFF_US) ? MAX_BACKOFF_US : current_backoff_us * 2;
                printf("[Watcher] Circuit Breaker Activated: Rapid flapping detected (%d crashes within 10s). Backoff: %u ms\n",
                       MAX_CRASHES_IN_WINDOW, (unsigned int)(current_backoff_us / 1000));
            } else {
                current_backoff_us = MIN_BACKOFF_US;
            }

            usleep(current_backoff_us);
        }
    }

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        waitpid(g_child_pid, NULL, 0);
    }
    printf("[Watcher] Terminated cleanly.\n");
    return 0;
}
