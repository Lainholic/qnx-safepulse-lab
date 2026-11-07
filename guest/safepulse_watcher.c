#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#define RESMGR_PATH "/proc/boot/safepulse_resmgr"

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int main(int argc, char **argv) {
    pid_t pid;
    int status;
    uint32_t restart_count = 0;
    uint64_t crash_time = 0;
    uint64_t restart_time = 0;

    (void)argc;
    (void)argv;

    printf("==================================================\n");
    printf(" Starting QNX SafePulse Self-Healing Watcher Daemon\n");
    printf(" High-Availability Process Supervisor (HAM Pattern)\n");
    printf(" Target Binary: %s\n", RESMGR_PATH);
    printf("==================================================\n");

    while (1) {
        printf("[Watcher] Spawning /dev/safepulse Resource Manager (Run #%u)...\n", restart_count + 1);
        restart_time = get_time_ms();

        pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[Watcher] Fork failed: %s\n", strerror(errno));
            sleep(1);
            continue;
        }

        if (pid == 0) {
            /* Child process: Execute the SafePulse Resource Manager */
            char *args[] = { RESMGR_PATH, NULL };
            execv(RESMGR_PATH, args);
            /* If execv returns, it failed */
            fprintf(stderr, "[Watcher-Child] execv failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        /* Parent process: Monitor child resource manager */
        printf("[Watcher] Supervising child PID %d (Device: /dev/safepulse)\n", pid);

        /* Wait for child process termination (Crash, SIGSEGV, or exit) */
        pid_t terminated_pid = waitpid(pid, &status, 0);
        crash_time = get_time_ms();

        if (terminated_pid > 0) {
            restart_count++;
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf("[Watcher] ALERT: /dev/safepulse CRASHED with signal %d (%s)!\n",
                       sig, (sig == 11) ? "SIGSEGV - Induced Fault" : "Signal");
                printf("[Watcher] Timestamp: %llu ms\n", crash_time);
                printf("[Watcher] Initiating Self-Healing Recovery...\n");
                printf("[Watcher] Retaining persistent POSIX Shared Memory (/safepulse_ringbuf)\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
            } else if (WIFEXITED(status)) {
                printf("[Watcher] /dev/safepulse exited with status %d\n", WEXITSTATUS(status));
            }

            /* Minimal delay for fault cleanup, then respawn instantly (< 5ms) */
            usleep(2000); /* 2ms fast recovery */
            printf("[Watcher] Recovery completed in %llu ms! Respawning service...\n",
                   (crash_time >= restart_time) ? (get_time_ms() - crash_time) : 0ULL);
        }
    }

    return 0;
}
