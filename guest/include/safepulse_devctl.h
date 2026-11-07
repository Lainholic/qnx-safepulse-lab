#ifndef SAFEPULSE_DEVCTL_H
#define SAFEPULSE_DEVCTL_H

#include "safepulse.h"
#include <devctl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t total_inspected;
    uint32_t total_violations;
    uint32_t ringbuffer_count;
    uint32_t drop_count;
    uint32_t last_violation_code;
    float    last_safety_score;
    uint32_t uptime_seconds;
} safepulse_stats_t;

/* Custom QNX devctl commands */
#define DCMD_SAFEPULSE_GET_STATS    __DIOF(_DCMD_MISC, 0x01, safepulse_stats_t)
#define DCMD_SAFEPULSE_RESET_STATS  __DION(_DCMD_MISC, 0x02)
#define DCMD_SAFEPULSE_INJECT_CHAOS __DIOT(_DCMD_MISC, 0x03, chaos_command_t)

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_DEVCTL_H */
