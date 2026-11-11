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

static void report(const char *operation, int err) {
    printf("%s: %s (errno=%d, %s)\n", operation,
           err == 0 ? "succeeded" : "failed", err, strerror(err));
}

int main(int argc, char **argv) {
    char *end = NULL;
    long target = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || *argv[1] == '\0' || *end != '\0' || target <= 1) {
        fprintf(stderr, "Usage: rogue_writer <resmgr-pid>\n");
        return 2;
    }

    int fd = open("/dev/safepulse", O_WRONLY);
    int open_error = fd < 0 ? errno : 0;
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
        report("write fake telemetry", n == (ssize_t)sizeof(pkt) ? 0 : (n < 0 ? errno : EIO));
        close(fd);
    } else {
        printf("write fake telemetry: not attempted because open failed\n");
    }

    fd = open("/dev/safepulse", O_RDWR);
    if (fd < 0) fd = open("/dev/safepulse", O_RDONLY);
    if (fd < 0) {
        report("open for reset devctl", errno);
    } else {
        int status = devctl(fd, DCMD_SAFEPULSE_RESET_STATS, NULL, 0, NULL);
        report("reset stats devctl", status);
        close(fd);
    }

    int status = kill((pid_t)target, SIGKILL);
    report("SIGKILL resmgr", status == 0 ? 0 : errno);
    return 0;
}
