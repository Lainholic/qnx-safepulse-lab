#ifndef SAFEPULSE_LINTER_H
#define SAFEPULSE_LINTER_H

#include "safepulse.h"
#include "autosar_e2e.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TRACKED_SENSORS 16U

/* Safety threshold boundaries */
#define LIMIT_STEERING_MIN  -540.0
#define LIMIT_STEERING_MAX   540.0
#define LIMIT_STEERING_JERK  720.0  /* max deg/sec rate of change */

#define LIMIT_BRAKE_MIN        0.0
#define LIMIT_BRAKE_MAX      150.0  /* bar */
#define LIMIT_BRAKE_JERK     300.0  /* bar/sec */

#define LIMIT_THROTTLE_MIN     0.0
#define LIMIT_THROTTLE_MAX   100.0  /* % */
#define LIMIT_THROTTLE_JERK  200.0  /* %/sec */

#define LIMIT_DEADLINE_NS    80000000ULL /* 80ms max allowable period (4x of 20ms period for ASIL-D jitter tolerance) */

typedef struct {
    double   min_bound;
    double   max_bound;
    double   max_jerk;
    uint64_t deadline_ns;
} sensor_rule_config_t;

typedef struct {
    uint64_t last_timestamp_ns;
    double   last_primary_val;
    uint32_t last_seq_num;
    bool     initialized;
} sensor_track_state_t;

typedef struct {
    sensor_rule_config_t rules[MAX_TRACKED_SENSORS];
    sensor_track_state_t sensors[MAX_TRACKED_SENSORS];
    e2e_channel_state_t  e2e_channels[MAX_TRACKED_SENSORS];
    uint32_t total_inspected;
    uint32_t total_violations;
} linter_state_t;

/* Linter operations */
void linter_init(linter_state_t *state);
void linter_inspect_packet(linter_state_t *state, safepulse_packet_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_LINTER_H */
