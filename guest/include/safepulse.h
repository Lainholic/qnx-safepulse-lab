#ifndef SAFEPULSE_H
#define SAFEPULSE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic bytes for SafePulse packets ("SPLS" in ASCII -> 0x53504C53) */
#define SAFEPULSE_MAGIC 0x53504C53U

/* Magic bytes for Chaos injection packets ("CHAO" in ASCII -> 0x4348414FU) */
#define CHAOS_MAGIC     0x4348414FU

/* Sensor Type Identifiers */
#define SENSOR_TYPE_STEERING    1U  /* Steering angle (-540.0 to +540.0 deg) */
#define SENSOR_TYPE_BRAKE       2U  /* Brake pressure (0.0 to 150.0 bar) */
#define SENSOR_TYPE_THROTTLE    3U  /* Throttle position (0.0 to 100.0 %) */
#define SENSOR_TYPE_IMU_YAW     4U  /* IMU Yaw rate (-100.0 to +100.0 deg/s) */

/* Status Flags (Bitmask) */
#define STATUS_FLAG_NORMAL      0x0000U
#define STATUS_FLAG_WARNING     0x0001U
#define STATUS_FLAG_CRITICAL    0x0002U
#define STATUS_FLAG_HAM_RECOVER 0x0004U

/* Safety Violation Codes */
#define VIOLATION_NONE          0x00000000U
#define VIOLATION_OUT_OF_BOUNDS 0x00001001U /* Sensor value outside physical limits */
#define VIOLATION_JERK_EXCEEDED 0x00001002U /* Rate of change exceeds safe threshold */
#define VIOLATION_DEADLINE_MISS 0x00001003U /* Sensor task missed periodic deadline */
#define VIOLATION_CRC_CORRUPTED 0x00001004U /* CRC32 checksum mismatch */

#pragma pack(push, 1)

/**
 * 64-byte Fixed-size Zero-Copy Telemetry Packet
 * Aligned and packed to ensure deterministic serialization across QNX and Host.
 */
typedef struct {
    uint32_t magic;           /* 0x00: 0x53504C53 ("SPLS") */
    uint32_t seq_num;         /* 0x04: Sequence counter */
    uint64_t timestamp_ns;    /* 0x08: High-precision timestamp (nanoseconds) */
    uint32_t task_id;         /* 0x10: Emitting Task/PID */
    uint16_t sensor_type;     /* 0x14: Sensor category */
    uint16_t status_flags;    /* 0x16: Health and recovery status flags */
    double   val_primary;     /* 0x18: Primary metric (e.g. angle, bar, %) */
    double   val_secondary;   /* 0x20: Secondary metric (e.g. rate, temp) */
    float    safety_score;    /* 0x28: Evaluated Safety Score (0.0 ~ 100.0) */
    uint32_t violation_code;  /* 0x2C: Detected violation code */
    uint8_t  reserved[12];    /* 0x30: Future expansion padding (zeroed) */
    uint32_t crc32;           /* 0x3C: CRC-32 checksum of bytes 0x00..0x3B */
} safepulse_packet_t;

/**
 * 16-byte Fault Injection Command (Host -> QNX)
 */
typedef struct {
    uint32_t magic;           /* 0x00: 0x4348414F ("CHAO") */
    uint32_t fault_type;      /* 0x04: 1=Spike, 2=Freeze/Deadline, 3=Crash */
    uint32_t duration_ms;     /* 0x08: Duration or magnitude */
    uint32_t checksum;        /* 0x0C: Simple XOR checksum */
} chaos_command_t;

#pragma pack(pop)

/* Compile-time verification that sizes are exact */
typedef char _check_packet_size[(sizeof(safepulse_packet_t) == 64) ? 1 : -1];
typedef char _check_chaos_size[(sizeof(chaos_command_t) == 16) ? 1 : -1];

/* Utility CRC32 computation */
uint32_t safepulse_crc32(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_H */
