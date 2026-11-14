#ifndef SAFEPULSE_CONTROL_H
#define SAFEPULSE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Actuator Command issued by SafePulse Safety Gateway to the Vehicle Plant / Actuators.
 */
typedef struct {
    uint64_t command_id;
    uint64_t issued_ns;
    uint64_t valid_until_ns;   /* QNX monotonic clock expiration */
    double   target_steering_deg;
    double   target_throttle_pct;
    double   target_brake_bar;
    double   speed_limit_kmh;
    uint32_t safety_state;      /* ASIL_STATE_NORMAL(0), DEGRADED(1), LIMP(2), ESTOP(3) */
    uint32_t violation_code;
    uint32_t flags;             /* 0x01: ESTOP_LATCHED, 0x02: ACTUATOR_RAMP_ACTIVE */
} sp_actuator_command_t;

#define ACTUATOR_FLAG_ESTOP_LATCHED      0x01U
#define ACTUATOR_FLAG_RAMP_ACTIVE        0x02U

/**
 * Real-time Vehicle Feedback reported by the Vehicle Plant to SafePulse Gateway.
 */
typedef struct {
    uint64_t applied_command_id;
    uint64_t plant_timestamp_ns;
    uint64_t received_ns;       /* QNX monotonic timestamp when received by gateway */
    double   actual_speed_kmh;
    double   actual_steering_deg;
    double   actual_brake_bar;
    double   stopping_distance_m;
    uint32_t status_flags;      /* 0x01: VEHICLE_STOPPED (< 0.05 km/h), 0x02: BRAKE_LOCKED */
} sp_vehicle_feedback_t;

#define VEHICLE_FEEDBACK_FLAG_STOPPED      0x01U
#define VEHICLE_FEEDBACK_FLAG_BRAKE_LOCKED 0x02U

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_CONTROL_H */
