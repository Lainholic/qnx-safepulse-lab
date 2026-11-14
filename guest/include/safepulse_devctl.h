#ifndef SAFEPULSE_DEVCTL_H
#define SAFEPULSE_DEVCTL_H

#include "safepulse.h"
#include "safepulse_control.h"
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
    uint32_t sched_policy;       /* SCHED_FIFO(1), SCHED_RR(2), SCHED_SPORADIC(4) */
    uint32_t sched_priority;     /* Active real-time priority (e.g. 50) */
    uint32_t fsm_state;          /* Safe state FSM (0=NORMAL, 1=DEGRADED, 2=LIMP, 3=ESTOP) */
    uint32_t max_inspect_ns;     /* Slowest inline inspection since last latency reset (ClockCycles) */
    uint32_t max_gap_ns;         /* Longest silence between sensor packets since last latency reset */
    uint32_t active_profile;     /* 0=PASSENGER(rules.json), 1=TRUCK(truck_rules.yaml), 2=CUSTOM */
    uint32_t starved_sensor;     /* 0=NONE/GLOBAL, 1=STEERING, 2=BRAKE, 3=THROTTLE */
    uint32_t starvation_limit_ms;/* Active global starvation watchdog timeout in ms (e.g. 50 or 100) */
} safepulse_stats_t;

#define SAFEPULSE_PROFILE_PASSENGER 0U /* /proc/boot/rules.json */
#define SAFEPULSE_PROFILE_TRUCK     1U /* /proc/boot/truck_rules.yaml */
#define SAFEPULSE_PROFILE_CUSTOM    2U /* Inline JSON/YAML via DCMD_SAFEPULSE_LOAD_RULES */

typedef struct {
    char payload[2048];
} safepulse_rules_blob_t;

/* Custom QNX devctl commands */
#define DCMD_SAFEPULSE_GET_STATS    __DIOF(_DCMD_MISC, 0x01, safepulse_stats_t)
#define DCMD_SAFEPULSE_RESET_STATS  __DION(_DCMD_MISC, 0x02)
#define DCMD_SAFEPULSE_INJECT_CHAOS __DIOT(_DCMD_MISC, 0x03, chaos_command_t)
#define DCMD_SAFEPULSE_SYNC_EDGE    __DION(_DCMD_MISC, 0x04)
#define DCMD_SAFEPULSE_GET_PID      __DIOF(_DCMD_MISC, 0x05, uint32_t)
#define DCMD_SAFEPULSE_RESET_LATENCY __DION(_DCMD_MISC, 0x06)
#define DCMD_SAFEPULSE_LOAD_PROFILE __DIOT(_DCMD_MISC, 0x07, uint32_t)
#define DCMD_SAFEPULSE_LOAD_RULES   __DIOT(_DCMD_MISC, 0x08, safepulse_rules_blob_t)
#define DCMD_SAFEPULSE_GET_COMMAND  __DIOF(_DCMD_MISC, 0x09, sp_actuator_command_t)
#define DCMD_SAFEPULSE_SEND_FEEDBACK __DIOT(_DCMD_MISC, 0x0A, sp_vehicle_feedback_t)

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_DEVCTL_H */
