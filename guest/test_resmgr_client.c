#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/iomgr.h>
#include <sys/siginfo.h>
#include "safepulse.h"
#include "safepulse_devctl.h"

int main(void) {
    int fd;
    int i;
    safepulse_packet_t pkt;
    safepulse_stats_t stats;
    int pass_count = 0;
    int total_tests = 5;

    printf("==================================================\n");
    printf(" Testing /dev/safepulse Resource Manager Client \n");
    printf("==================================================\n");

    /* TEST 1: Open /dev/safepulse */
    printf("[TEST 1] Opening /dev/safepulse... ");
    fd = open("/dev/safepulse", O_RDWR);
    if (fd >= 0) {
        printf("PASS (fd = %d)\n", fd);
        pass_count++;
    } else {
        printf("FAIL: %s\n", strerror(errno));
        return 1;
    }

    /* TEST 2: Write 5 normal packets */
    printf("[TEST 2] Writing 5 telemetry packets... ");
    for (i = 0; i < 5; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.seq_num = i;
        pkt.timestamp_ns = 1000000000ULL + (uint64_t)i * 10000000ULL;
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.val_primary = 10.0 + (double)i;
        pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));

        if (write(fd, &pkt, sizeof(pkt)) != sizeof(pkt)) {
            printf("FAIL on write %d: %s\n", i, strerror(errno));
            close(fd);
            return 1;
        }
    }
    printf("PASS (5 packets written)\n");
    pass_count++;

    /* TEST 3: Write 1 Out-of-bounds packet (+999 deg steering) */
    printf("[TEST 3] Writing Out-of-bounds packet (+999 deg)... ");
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.seq_num = 5;
    pkt.timestamp_ns = 1000000000ULL + 5ULL * 10000000ULL;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.val_primary = 999.0; /* Out of bounds */
    pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
    if (write(fd, &pkt, sizeof(pkt)) == sizeof(pkt)) {
        printf("PASS (Anomaly written)\n");
        pass_count++;
    } else {
        printf("FAIL: %s\n", strerror(errno));
    }

    /* TEST 4: Query devctl stats */
    printf("[TEST 4] Querying DCMD_SAFEPULSE_GET_STATS via devctl... ");
    memset(&stats, 0, sizeof(stats));
    if (devctl(fd, DCMD_SAFEPULSE_GET_STATS, &stats, sizeof(stats), NULL) == 0) {
        printf("PASS\n");
        printf("  - Total Inspected: %u (expected 6)\n", stats.total_inspected);
        printf("  - Total Violations: %u (expected 1)\n", stats.total_violations);
        printf("  - Last Violation Code: 0x%X (expected 0x1001)\n", stats.last_violation_code);
        printf("  - Last Safety Score: %.1f\n", stats.last_safety_score);
        if (stats.total_inspected == 6 && stats.total_violations == 1 && stats.last_violation_code == 0x00001001) {
            pass_count++;
        } else {
            printf("FAIL: Stats mismatch\n");
        }
    } else {
        printf("FAIL: %s\n", strerror(errno));
    }

    /* TEST 5: Verify QNX _IO_NOTIFY (ionotify + poll) readiness notification */
    printf("[TEST 5] Verifying QNX _IO_NOTIFY readiness notification... ");
    (void)fcntl(fd, F_SETFL, O_NONBLOCK);
    while (read(fd, &pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt)) {
        /* Drain queued packets */
    }
    struct sigevent sev;
    SIGEV_NONE_INIT(&sev);
    int nflags = ionotify(fd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &sev);
    if (nflags < 0 || (nflags & _NOTIFY_COND_INPUT) != 0) {
        printf("FAIL: unexpected initial ionotify flags=%d errno=%d\n", nflags, errno);
    } else {
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.seq_num = 6;
        pkt.timestamp_ns = 1000000000ULL + 6ULL * 10000000ULL;
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.val_primary = 12.0;
        pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
        if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
            printf("FAIL: write before poll failed: %s\n", strerror(errno));
        } else {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 50);
            safepulse_packet_t rx_pkt;
            ssize_t rr = read(fd, &rx_pkt, sizeof(rx_pkt));
            if (pr == 1 && (pfd.revents & POLLIN) != 0 && rr == (ssize_t)sizeof(rx_pkt)) {
                printf("PASS (_IO_NOTIFY woke poll)\n");
                pass_count++;
            } else {
                printf("FAIL: poll=%d revents=0x%x read=%zd\n", pr, (unsigned)pfd.revents, rr);
            }
        }
    }

    close(fd);

    printf("==================================================\n");
    printf(" Client Result: %d / %d Tests Passed\n", pass_count, total_tests);
    printf("==================================================\n");
    return (pass_count == total_tests) ? 0 : 1;
}
