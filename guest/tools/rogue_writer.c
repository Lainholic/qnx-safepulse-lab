#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "safepulse.h"
#include "safepulse_devctl.h"
#include "autosar_e2e.h"

static int wire_mode = 0;
static void report(const char *operation, int err) {
    if (!wire_mode) printf("%s: %s (errno=%d, %s)\n", operation,
                           err == 0 ? "succeeded" : "failed", err, strerror(err));
}

int main(int argc, char **argv) {
    wire_mode = argc == 3 && strcmp(argv[1], "--wire") == 0;
    const char *target_arg = argc == (wire_mode ? 3 : 2) ? argv[wire_mode ? 2 : 1] : "";
    char *end = NULL;
    long target = strtol(target_arg, &end, 10);
    if (*target_arg == '\0' || *end != '\0' || target <= 1) {
        fprintf(stderr, "Usage: rogue_writer [--wire] <resmgr-pid>\n");
        return 2;
    }
    safepulse_rogue_result_t result = {0};
    result.magic = SAFEPULSE_ROGUE_RESULT_MAGIC;
    result.write_error = result.reset_error = -1;

    int fd = open("/dev/safepulse", O_WRONLY);
    int open_error = fd < 0 ? errno : 0;
    result.open_write_error = open_error;
    report("open for fake telemetry", open_error);
    if (fd >= 0) {
        struct timespec now;
        safepulse_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        clock_gettime(CLOCK_MONOTONIC, &now);
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.seq_num = 1;
        pkt.timestamp_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        pkt.task_id = (uint32_t)getpid();
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.safety_score = 100.0f;
        autosar_e2e_stamp_packet(&pkt, 1);
        pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
        ssize_t n = write(fd, &pkt, sizeof(pkt));
        result.write_error = n == (ssize_t)sizeof(pkt) ? 0 : (n < 0 ? errno : EIO);
        report("write fake telemetry", result.write_error);
        close(fd);
    } else {
        if (!wire_mode) printf("write fake telemetry: not attempted because open failed\n");
    }

    fd = open("/dev/safepulse", O_RDWR);
    if (fd < 0) fd = open("/dev/safepulse", O_RDONLY);
    if (fd < 0) {
        result.open_reset_error = errno;
        report("open for reset devctl", result.open_reset_error);
    } else {
        int status = devctl(fd, DCMD_SAFEPULSE_RESET_STATS, NULL, 0, NULL);
        result.reset_error = status;
        report("reset stats devctl", status);
        close(fd);
    }

    int status = kill((pid_t)target, SIGKILL);
    result.signal_error = status == 0 ? 0 : errno;
    report("SIGKILL resmgr", result.signal_error);
    if (wire_mode && write(STDOUT_FILENO, &result, sizeof(result)) != (ssize_t)sizeof(result)) return 1;
    return 0;
}
