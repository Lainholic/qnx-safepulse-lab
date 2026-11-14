#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "safepulse_devctl.h"

int main(void) {
    int fd = open("/dev/safepulse", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open stats: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    safepulse_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    int status = devctl(fd, DCMD_SAFEPULSE_GET_STATS, &stats, sizeof(stats), NULL);
    close(fd);
    if (status != 0) {
        fprintf(stderr, "GET_STATS: error=%d (%s)\n", status, strerror(status));
        return 1;
    }
    printf("GET_STATS: state=%u inspected=%u violations=%u last_violation=%u profile=%u starved=%u watchdog_ms=%u\n",
           stats.fsm_state, stats.total_inspected, stats.total_violations,
           stats.last_violation_code, stats.active_profile, stats.starved_sensor,
           stats.starvation_limit_ms);
    return 0;
}
