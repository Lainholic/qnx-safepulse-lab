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
#define SAFEPULSE_ROGUE_RESULT_MAGIC 0x524F4755U
#define SAFEPULSE_LOAD_RESULT_MAGIC  0x4C4F4144U /* "LOAD" */
#define SAFEPULSE_PROC_MAGIC         0x50524F43U /* "PROC" */
#define SAFEPULSE_TIMING_MAGIC       0x54494D45U /* "TIME" */

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
#define VIOLATION_NONE           0x00000000U
#define VIOLATION_OUT_OF_BOUNDS  0x00001001U /* Sensor value outside physical limits */
#define VIOLATION_JERK_EXCEEDED  0x00001002U /* Rate of change exceeds safe threshold */
#define VIOLATION_DEADLINE_MISS  0x00001003U /* Sensor task missed periodic deadline */
#define VIOLATION_CRC_CORRUPTED  0x00001004U /* CRC32 / AUTOSAR E2E CRC mismatch */
#define VIOLATION_PLAUSIBILITY   0x00001005U /* Redundant sensor contradiction or E2E Data ID fault */
#define VIOLATION_E2E_REPEATED   0x00001006U /* AUTOSAR E2E Alive Counter repeated (freeze/replay) */
#define VIOLATION_E2E_SEQ_LOSS   0x00001007U /* AUTOSAR E2E Alive Counter jump exceeded (sequence loss) */
#define VIOLATION_STARVATION     0x00001008U /* Total ECU bus silence >= STARVATION_LIMIT_NS (50ms) */
#define VIOLATION_CRASH_SIGSEGV  0x00001009U /* Resource manager SIGSEGV fault & HAM respawn */
#define VIOLATION_CRASH_SIGFPE   0x0000100AU /* Resource manager SIGFPE fault & HAM respawn */
#define VIOLATION_CRASH_SIGABRT  0x0000100BU /* Resource manager SIGABRT fault & HAM respawn */
#define VIOLATION_CRASH_SIGILL   0x0000100CU /* Resource manager SIGILL fault & HAM respawn */

/* AUTOSAR E2E Profile 1 & Kernel Runtime State Layout in safepulse_packet_t.reserved[0..11]
 * reserved[0..1] : 16-bit AUTOSAR E2E Data ID (little-endian)
 * reserved[2]    : 4-bit AUTOSAR E2E Alive Counter (0..15)
 * reserved[3]    : 0xE2 Marker (SAFEPULSE_E2E_MARKER)
 * reserved[4..7] : 32-bit AUTOSAR E2E CRC-32 (0xF4ACFB13)
 * reserved[8]    : Actual QNX Safe State FSM (0=NORMAL, 1=DEGRADED, 2=LIMP_HOME, 3=EMERGENCY_STOP)
 * reserved[9]    : Actual QNX Kernel Scheduler Policy (1=SCHED_FIFO, 2=SCHED_RR, 4=SCHED_SPORADIC)
 * reserved[10]   : Actual QNX Real-Time Priority (e.g. 50)
 * reserved[11]   : 0xA5 Kernel State Marker (SAFEPULSE_STATE_MARKER)
 */
#define SAFEPULSE_E2E_MARKER          0xE2U
#define SAFEPULSE_STATE_MARKER        0xA5U
#define SAFEPULSE_E2E_DATA_ID_BASE    0x1000U
#define SAFEPULSE_E2E_PAYLOAD_LEN     offsetof(safepulse_packet_t, safety_score) /* 40 bytes (0x00..0x27) */

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

typedef struct {
    uint32_t magic;
    int32_t open_write_error;
    int32_t write_error;
    int32_t open_reset_error;
    int32_t reset_error;
    int32_t signal_error;
    int32_t launch_error;
    uint8_t reserved[36];
} safepulse_rogue_result_t;

/* Result of the QM CPU-overload (freedom-from-interference) run, sent to the host as one 64-byte frame. */
typedef struct {
    uint32_t magic;               /* SAFEPULSE_LOAD_RESULT_MAGIC */
    uint32_t duration_ms;         /* How long the QM load ran */
    uint32_t threads;             /* QM load threads started */
    uint32_t load_priority;       /* QM thread priority (below every safety task) */
    uint64_t load_iterations;     /* Work done by the QM threads (proves they really ran) */
    uint32_t packets_inspected;   /* Packets /dev/safepulse inspected during the load */
    uint32_t violations;          /* Violations raised during the load */
    uint32_t max_inspect_ns;      /* Slowest inline inspection during the load */
    uint32_t max_gap_ns;          /* Longest sensor silence during the load */
    uint32_t fsm_state;           /* FSM state when the load ended */
    uint32_t safety_priority;     /* /dev/safepulse scheduler priority */
    int32_t  error;               /* 0, or errno if the load could not start */
    uint8_t  reserved[12];
} safepulse_load_result_t;

/* One row of the live QNX process view (read from /proc by the bridge), one 64-byte frame per process. */
typedef struct {
    uint32_t magic;               /* SAFEPULSE_PROC_MAGIC */
    uint32_t cycle;               /* Snapshot number; rows of one snapshot share it */
    uint16_t index;               /* Row index within the snapshot */
    uint16_t count;               /* Rows in the snapshot */
    int32_t  pid;
    uint16_t threads;
    uint8_t  prio_max;            /* Highest thread priority */
    uint8_t  prio_min;            /* Lowest thread priority */
    uint8_t  policy_max;          /* Scheduling policy of the highest-priority thread */
    uint8_t  state_main;          /* Kernel state of the first thread */
    uint8_t  policy_min;          /* Scheduling policy of the lowest-priority thread */
    uint8_t  pad;
    uint64_t cpu_ns;              /* Total user + system CPU time */
    char     name[24];
    uint8_t  reserved[8];
} safepulse_proc_entry_t;

/* Worst-case timing of /dev/safepulse over the last window (about 1 s), measured inside QNX. */
typedef struct {
    uint32_t magic;               /* SAFEPULSE_TIMING_MAGIC */
    uint32_t window_ms;           /* Length of the window these maxima cover */
    uint32_t max_inspect_ns;      /* Slowest inline inspection in the window */
    uint32_t max_gap_ns;          /* Longest silence between sensor packets in the window */
    uint32_t inspected;           /* Packets inspected in the window */
    uint32_t fsm_state;
    uint32_t load_active;         /* 1 while the QM CPU overload run is going */
    uint8_t  reserved[36];
} safepulse_timing_t;

#pragma pack(pop)

/* Compile-time verification that sizes are exact */
typedef char _check_packet_size[(sizeof(safepulse_packet_t) == 64) ? 1 : -1];
typedef char _check_chaos_size[(sizeof(chaos_command_t) == 16) ? 1 : -1];
typedef char _check_rogue_result_size[(sizeof(safepulse_rogue_result_t) == 64) ? 1 : -1];
typedef char _check_load_result_size[(sizeof(safepulse_load_result_t) == 64) ? 1 : -1];
typedef char _check_proc_entry_size[(sizeof(safepulse_proc_entry_t) == 64) ? 1 : -1];
typedef char _check_timing_size[(sizeof(safepulse_timing_t) == 64) ? 1 : -1];

/* Utility CRC32 computation */
uint32_t safepulse_crc32(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_H */
