#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdatomic.h>

#include "safepulse.h"
#include "safepulse_devctl.h"
#include "ringbuffer.h"
#include "linter.h"
#include "safe_state_fsm.h"

#define RESMGR_DEVICE_NAME "/dev/safepulse"
#define SHM_NAME           "/safepulse_ringbuf"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          ioattr;

static ringbuffer_t    *g_rb = NULL;
static linter_state_t   g_linter;
static safe_state_fsm_t g_fsm;
static actuator_slew_state_t g_slew;
static safepulse_stats_t g_stats;
static time_t           g_start_time = 0;
static uint8_t          g_active_sched_policy = 0;
static uint8_t          g_active_sched_prio   = 0;

static int safepulse_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status;
    safepulse_packet_t pkt;

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;
    safe_state_fsm_check_starvation(&g_fsm, now_ns);

    if ((status = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK) {
        return status;
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    if (msg->i.nbytes < (int)sizeof(safepulse_packet_t)) {
        return EINVAL;
    }

    if (ringbuffer_pop(g_rb, &pkt)) {
        MsgReply(ctp->rcvid, sizeof(pkt), &pkt, sizeof(pkt));
        return _RESMGR_NOREPLY;
    } else {
        /* No new packet in ringbuffer: return 0 bytes */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
}

static int safepulse_io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    int status;
    const int nbytes = msg->i.nbytes;

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;

    if ((status = iofunc_write_verify(ctp, msg, ocb, NULL)) != EOK) {
        return status;
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    /* Strict size validation: Reject malformed buffer sizes */
    if (nbytes == (int)sizeof(safepulse_packet_t)) {
        safe_state_fsm_check_starvation(&g_fsm, now_ns);
        safepulse_packet_t pkt;
        if (resmgr_msgget(ctp, &pkt, sizeof(pkt), sizeof(msg->i)) < 0) {
            return EFAULT;
        }

        /* Inline Real-time Safety Linter Inspection (re-entrant) */
        linter_inspect_packet(&g_linter, &pkt);

        /* Step ISO 26262 ASIL-D Safe State Transition FSM */
        asil_safe_state_t cur_state = safe_state_fsm_step(&g_fsm, pkt.violation_code, pkt.safety_score, pkt.timestamp_ns);
        /* Anchor starvation heartbeat strictly to QNX kernel monotonic clock domain */
        g_fsm.last_packet_timestamp_ns = now_ns;
        if (g_rb) {
            atomic_store_explicit(&g_rb->persistent_fsm_state, (uint32_t)cur_state, memory_order_relaxed);
            atomic_store_explicit(&g_rb->persistent_fsm_latched, g_fsm.is_latched ? 1U : 0U, memory_order_relaxed);
            atomic_store_explicit(&g_rb->persistent_latch_time, g_fsm.state_entered_ns, memory_order_relaxed);
            uint32_t crc_payload[3] = {
                (uint32_t)cur_state,
                g_fsm.is_latched ? 1U : 0U,
                (uint32_t)(g_fsm.state_entered_ns & 0xFFFFFFFFU)
            };
            uint32_t crc = safepulse_crc32((const uint8_t*)crc_payload, sizeof(crc_payload));
            atomic_store_explicit(&g_rb->persistent_crc32, crc, memory_order_relaxed);
        }

        if (cur_state != ASIL_STATE_NORMAL) {
            actuator_limits_t lim = safe_state_get_limits(cur_state);
            /* Smooth Ramped Slew-Rate Clamping (ISO 26262-4 / 6 Anti-Jerk Rollover Protection) */
            if (pkt.sensor_type == SENSOR_TYPE_STEERING) {
                pkt.val_primary = actuator_slew_clamp_steering(&g_slew, pkt.val_primary, lim.max_steering_angle, pkt.timestamp_ns);
            } else if (pkt.sensor_type == SENSOR_TYPE_THROTTLE) {
                pkt.val_primary = actuator_slew_clamp_throttle(&g_slew, pkt.val_primary, lim.max_throttle_pct, pkt.timestamp_ns);
            } else if (pkt.sensor_type == SENSOR_TYPE_BRAKE) {
                /* ASIL-D Safe Braking: Enforce minimum deceleration brake pressure (e.g. 100 bar on E-STOP) */
                if (pkt.val_primary < lim.min_brake_bar) {
                    pkt.val_primary = lim.min_brake_bar;
                }
            }
        }

        /* Stamp authoritative QNX kernel FSM state & active scheduler policy into reserved[8..11] */
        pkt.reserved[8]  = (uint8_t)cur_state;
        pkt.reserved[9]  = g_active_sched_policy;
        pkt.reserved[10] = g_active_sched_prio;
        pkt.reserved[11] = SAFEPULSE_STATE_MARKER;
        pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));

        /* Push verified packet to Lock-Free Zero-Copy 2MB Ringbuffer */
        ringbuffer_push(g_rb, &pkt);
        atomic_thread_fence(memory_order_release);

        /* Atomic statistics updates (Strict aliasing compliant) */
        atomic_fetch_add_explicit((_Atomic uint32_t*)&g_stats.total_inspected, 1, memory_order_relaxed);
        if (pkt.violation_code != VIOLATION_NONE) {
            atomic_fetch_add_explicit((_Atomic uint32_t*)&g_stats.total_violations, 1, memory_order_relaxed);
            atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, pkt.violation_code, memory_order_relaxed);
        }
        uint32_t score_bits = 0;
        memcpy(&score_bits, &pkt.safety_score, sizeof(score_bits));
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_safety_score, score_bits, memory_order_relaxed);

    } else if (nbytes == (int)sizeof(chaos_command_t)) {
        chaos_command_t cmd;
        if (resmgr_msgget(ctp, &cmd, sizeof(cmd), sizeof(msg->i)) < 0) {
            return EFAULT;
        }

        if (cmd.magic == CHAOS_MAGIC) {
            printf("[SafePulse] Injected Chaos Command Type: %u, Duration: %u ms\n",
                   cmd.fault_type, cmd.duration_ms);
            if (cmd.fault_type == 0) {
                printf("[SafePulse] Operator Command: Clearing Fault Latch -> Restoring ASIL_STATE_NORMAL\n");
                safe_state_fsm_init(&g_fsm, now_ns);
                linter_init(&g_linter);
                actuator_slew_init(&g_slew);
                if (g_rb) {
                    atomic_store_explicit(&g_rb->persistent_fsm_state, (uint32_t)ASIL_STATE_NORMAL, memory_order_relaxed);
                    atomic_store_explicit(&g_rb->persistent_fsm_latched, 0U, memory_order_relaxed);
                }
            } else if (cmd.fault_type == 3) {
                printf("[SafePulse] Triggering Induced Fault Crash (SIGSEGV - Null Pointer Dereference)...\n");
                volatile int *null_ptr = NULL;
                *null_ptr = 0xDEAD; /* Induce SIGSEGV to verify self-healing recovery */
            } else if (cmd.fault_type == 6) {
                printf("[SafePulse] Triggering Induced Fault Crash (SIGFPE - Arithmetic Divide by Zero)...\n");
                volatile int zero = 0;
                volatile int res = 100 / zero;
                (void)res;
            } else if (cmd.fault_type == 7) {
                printf("[SafePulse] Triggering Induced Fault Crash (SIGABRT - Critical Assertion Abort)...\n");
                abort();
            } else if (cmd.fault_type == 8) {
                printf("[SafePulse] Triggering Induced Fault Crash (SIGILL - Illegal Opcode Execution)...\n");
                __builtin_trap();
            }
        }
    } else {
        return EMSGSIZE;
    }

    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static int safepulse_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status;

    if ((status = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT) {
        return status;
    }

    switch (msg->i.dcmd) {
        case DCMD_SAFEPULSE_GET_STATS: {
            if (!(ocb->ioflag & _IO_FLAG_RD)) {
                return EPERM;
            }
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;
            safe_state_fsm_check_starvation(&g_fsm, now_ns);

            safepulse_stats_t *out = (safepulse_stats_t *)_DEVCTL_DATA(msg->i);
            g_stats.ringbuffer_count = ringbuffer_count(g_rb);
            g_stats.drop_count = g_rb->drop_count;
            g_stats.uptime_seconds = (uint32_t)(time(NULL) - g_start_time);

            g_stats.sched_policy = (uint32_t)g_active_sched_policy;
            g_stats.sched_priority = (uint32_t)g_active_sched_prio;
            g_stats.fsm_state = (uint32_t)g_fsm.current_state;

            *out = g_stats;
            memset(&msg->o, 0, sizeof(msg->o));
            msg->o.nbytes = sizeof(safepulse_stats_t);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(safepulse_stats_t));
        }
        case DCMD_SAFEPULSE_RESET_STATS: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            memset(&g_stats, 0, sizeof(g_stats));
            g_rb->drop_count = 0;
            return EOK;
        }
        case DCMD_SAFEPULSE_SYNC_EDGE: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            g_rb->tail = g_rb->head;
            return EOK;
        }
        default:
            return ENOSYS;
    }
}

/* =========================================================================
 * ISO 26262 ASIL-D Power-On Built-In Self-Test (BIST / POST)
 * Runs 7 comprehensive unit tests on safety invariants before /dev/safepulse
 * is exposed to external processes. Fails fast if any latent fault is found.
 * ========================================================================= */
static int safepulse_run_bist(void) {
    printf("[BIST] Commencing ISO 26262 ASIL-D Power-On Self-Test (BIST)...\n");
    linter_state_t test_linter;
    safe_state_fsm_t test_fsm;
    safepulse_packet_t pkt;
    uint64_t t0 = 1000000000ULL;

    /* BIST-1: Linter initialization invariant */
    linter_init(&test_linter);
    if (test_linter.total_inspected != 0 || test_linter.total_violations != 0) {
        fprintf(stderr, "[BIST FAIL] Linter init state non-zero!\n");
        return -1;
    }
    printf("  [BIST 1/7] Linter State Initialization: PASS\n");

    /* BIST-2: Nominal packet admission (Score 100.0, Violation 0) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 1;
    pkt.timestamp_ns = t0;
    pkt.val_primary = 15.0;
    pkt.val_secondary = 15.0;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_NONE || pkt.safety_score < 99.0f) {
        fprintf(stderr, "[BIST FAIL] Nominal packet rejected! Code: 0x%04X, Score: %.1f\n",
                pkt.violation_code, pkt.safety_score);
        return -2;
    }
    printf("  [BIST 2/7] Nominal Sensor Validation & CRC Verification: PASS\n");

    /* BIST-3: Physical Out-of-Bounds Detection (0x1001) */
    linter_init(&test_linter);
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 2;
    pkt.timestamp_ns = t0 + 20000000ULL;
    pkt.val_primary = 850.0; /* Exceeds +/- 540.0 deg */
    pkt.val_secondary = 850.0;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_OUT_OF_BOUNDS) {
        fprintf(stderr, "[BIST FAIL] Out-of-bounds steering not caught! Code: 0x%04X\n", pkt.violation_code);
        return -3;
    }
    printf("  [BIST 3/7] Rule 1 Physical Bounds (0x1001) Enforcement: PASS\n");

    /* BIST-4: Plausibility Discrepancy Detection (0x1005: Extreme throttle with zero speed) */
    linter_init(&test_linter); /* Reset tracking state for isolated test */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_THROTTLE;
    pkt.seq_num = 3;
    pkt.timestamp_ns = t0 + 40000000ULL;
    pkt.val_primary = 95.0;  /* 95% throttle (> 85%) */
    pkt.val_secondary = 0.0; /* 0.0 km/h speed (< 1.0) -> Plausibility fault */
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_PLAUSIBILITY) {
        fprintf(stderr, "[BIST FAIL] Plausibility discrepancy not caught! Code: 0x%04X\n", pkt.violation_code);
        return -4;
    }
    printf("  [BIST 4/7] Rule 5 Multi-Sensor Plausibility (0x1005) Detection: PASS\n");

    /* BIST-5: CRC-32 Corruption Detection (0x1004) */
    linter_init(&test_linter); /* Reset tracking state */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 4;
    pkt.timestamp_ns = t0 + 60000000ULL;
    pkt.val_primary = 0.0;
    pkt.val_secondary = 0.0;
    pkt.crc32 = 0xDEADBEEF; /* Corrupted CRC */
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_CRC_CORRUPTED) {
        fprintf(stderr, "[BIST FAIL] CRC corruption not caught! Code: 0x%04X\n", pkt.violation_code);
        return -5;
    }
    printf("  [BIST 5/7] Rule 4 Hardware CRC-32 Corruption (0x1004) Trap: PASS\n");

    /* BIST-6: Dual-Rail FSM Initialization & Normal Step */
    safe_state_fsm_init(&test_fsm, t0);
    if (test_fsm.current_state != ASIL_STATE_NORMAL || test_fsm.shadow_state != (asil_safe_state_t)(~ASIL_STATE_NORMAL)) {
        fprintf(stderr, "[BIST FAIL] FSM dual-rail shadow register inverted mismatch!\n");
        return -6;
    }
    printf("  [BIST 6/7] Dual-Rail SEU Inverted Shadow State Redundancy: PASS\n");

    /* BIST-7: FSM Upward Escalation & Latching Guarantee */
    safe_state_fsm_step(&test_fsm, VIOLATION_CRC_CORRUPTED, 0.0f, t0 + 1000000ULL);
    if (test_fsm.current_state != ASIL_STATE_EMERGENCY_STOP || !test_fsm.is_latched) {
        fprintf(stderr, "[BIST FAIL] Catastrophic fault did not escalate to E-STOP latch!\n");
        return -7;
    }
    printf("  [BIST 7/7] Unconditional ASIL-D Upward Escalation to E-STOP Latch: PASS\n");

    /* BIST-8: AUTOSAR E2E Profile 1 Inline Verification (Normal, Bitflip, Skip, Repeat) */
    linter_init(&test_linter);
    for (uint8_t c = 0; c < 2; c++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = SAFEPULSE_MAGIC;
        pkt.sensor_type = SENSOR_TYPE_STEERING;
        pkt.seq_num = 10U + c;
        pkt.timestamp_ns = t0 + (uint64_t)(c + 1) * 20000000ULL;
        pkt.val_primary = 10.0;
        autosar_e2e_stamp_packet(&pkt, c);
        pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
        linter_inspect_packet(&test_linter, &pkt);
        if (pkt.violation_code != VIOLATION_NONE) {
            fprintf(stderr, "[BIST FAIL] Normal AUTOSAR E2E packet rejected! Code: 0x%04X\n", pkt.violation_code);
            return -8;
        }
    }
    /* Repeat alive counter 1 -> VIOLATION_E2E_REPEATED (0x1006) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 12U;
    pkt.timestamp_ns = t0 + 60000000ULL;
    pkt.val_primary = 10.0;
    autosar_e2e_stamp_packet(&pkt, 1U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_E2E_REPEATED) {
        fprintf(stderr, "[BIST FAIL] AUTOSAR E2E repeated counter not caught! Code: 0x%04X\n", pkt.violation_code);
        return -9;
    }
    /* Counter jump 1 -> 6 (delta 5 > max 2) -> VIOLATION_E2E_SEQ_LOSS (0x1007) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 13U;
    pkt.timestamp_ns = t0 + 80000000ULL;
    pkt.val_primary = 10.0;
    autosar_e2e_stamp_packet(&pkt, 6U);
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_E2E_SEQ_LOSS) {
        fprintf(stderr, "[BIST FAIL] AUTOSAR E2E counter skip not caught! Code: 0x%04X\n", pkt.violation_code);
        return -10;
    }
    /* Bitflip in E2E CRC -> VIOLATION_CRC_CORRUPTED (0x1004) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 14U;
    pkt.timestamp_ns = t0 + 100000000ULL;
    pkt.val_primary = 10.0;
    autosar_e2e_stamp_packet(&pkt, 7U);
    pkt.reserved[4] ^= 0x01U;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_CRC_CORRUPTED) {
        fprintf(stderr, "[BIST FAIL] AUTOSAR E2E CRC bitflip not caught! Code: 0x%04X\n", pkt.violation_code);
        return -11;
    }
    printf("  [BIST 8/8] AUTOSAR E2E Profile 1 Inline Verification (Normal/Bitflip/Skip/Repeat): PASS\n");

    printf("[BIST] ALL 8 ASIL-D POWER-ON SELF-TESTS PASSED (0 Latent Faults Detected).\n\n");
    return 0;
}

int main(int argc, char **argv) {
    dispatch_t           *dpp;
    dispatch_context_t   *ctp;
    resmgr_attr_t         resmgr_attr;
    int                   resmgr_id;
    int                   shm_fd;
    int                   is_new_shm = 0;

    (void)argc;
    (void)argv;

    g_start_time = time(NULL);

    /* Disable blocking disk core dumps; ASIL-D Blackbox Flight Recorder resides in zero-copy POSIX SHM */
    struct rlimit rl_core = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rl_core);

    printf("==================================================\n");
    printf(" Starting QNX SafePulse Lab Resource Manager \n");
    printf(" Device Path: %s\n", RESMGR_DEVICE_NAME);
    printf("==================================================\n");

    /* 0. Execute ISO 26262 ASIL-D Built-In Self-Test (BIST) before exposing interface */
    if (safepulse_run_bist() != 0) {
        fprintf(stderr, "[FATAL] SafePulse BIST self-test failed! Refusing to register %s.\n", RESMGR_DEVICE_NAME);
        return EXIT_FAILURE;
    }

    /* 1. Attach/Create Persistent Shared Memory (Fault-Isolation Survival) */
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (shm_fd >= 0) {
        is_new_shm = 1;
        if (ftruncate(shm_fd, sizeof(ringbuffer_t)) != 0) {
            perror("ftruncate shm");
            return EXIT_FAILURE;
        }
    } else if (errno == EEXIST) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd < 0) {
            perror("shm_open existing");
            return EXIT_FAILURE;
        }
        printf("[SafePulse] Reconnected to existing SHM buffer (Recovered from crash!)\n");
    } else {
        perror("shm_open");
        return EXIT_FAILURE;
    }

    g_rb = (ringbuffer_t *)mmap(NULL, sizeof(ringbuffer_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_rb == MAP_FAILED) {
        perror("mmap shm");
        return EXIT_FAILURE;
    }

    struct timespec ts_boot;
    clock_gettime(CLOCK_MONOTONIC, &ts_boot);
    uint64_t boot_time_ns = ((uint64_t)ts_boot.tv_sec * 1000000000ULL) + (uint64_t)ts_boot.tv_nsec;

    if (is_new_shm) {
        ringbuffer_init(g_rb);
        safe_state_fsm_init(&g_fsm, boot_time_ns);
        printf("[SafePulse] Initialized new 2MB Ringbuffer & Nominal FSM in Shared Memory.\n");
    } else {
        uint32_t prev_state   = atomic_load_explicit(&g_rb->persistent_fsm_state, memory_order_relaxed);
        uint32_t prev_latched = atomic_load_explicit(&g_rb->persistent_fsm_latched, memory_order_relaxed);
        uint64_t prev_time    = atomic_load_explicit(&g_rb->persistent_latch_time, memory_order_relaxed);
        uint32_t stored_crc   = atomic_load_explicit(&g_rb->persistent_crc32, memory_order_relaxed);

        uint32_t check_payload[3] = {
            prev_state,
            prev_latched,
            (uint32_t)(prev_time & 0xFFFFFFFFU)
        };
        uint32_t expected_crc = safepulse_crc32((const uint8_t*)check_payload, sizeof(check_payload));

        if (stored_crc == expected_crc && prev_latched && prev_state <= (uint32_t)ASIL_STATE_EMERGENCY_STOP) {
            safe_state_fsm_restore(&g_fsm, (asil_safe_state_t)prev_state, prev_time, true);
        } else if (!is_new_shm && prev_latched) {
            /* Fail-Safe Recovery: If CRC was perturbed during crash, default to EMERGENCY_STOP */
            printf("[SafePulse] Warning: Persistent state CRC mismatch! Fail-Safe escalation to EMERGENCY_STOP.\n");
            safe_state_fsm_force_estop(&g_fsm, boot_time_ns);
        } else {
            safe_state_fsm_init(&g_fsm, boot_time_ns);
        }
        uint32_t total_pushed  = atomic_load_explicit(&g_rb->total_pushed, memory_order_acquire);
        uint32_t retained_hist = (total_pushed < RINGBUFFER_CAPACITY_PACKETS) ? total_pushed : RINGBUFFER_CAPACITY_PACKETS;
        uint32_t unread_queued = ringbuffer_count(g_rb);
        uint32_t dropped_cnt   = atomic_load_explicit(&g_rb->drop_count, memory_order_acquire);
        printf("[SafePulse] SHM Ringbuffer Intact: Retained %u historical packets in 2MB SHM (%u unread, %u dropped).\n",
               retained_hist, unread_queued, dropped_cnt);
    }

    linter_init(&g_linter);
    actuator_slew_init(&g_slew);
    memset(&g_stats, 0, sizeof(g_stats));

    /* 2. Initialize QNX Microkernel Dispatch & Device Node */
    dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "dispatch_create failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    memset(&resmgr_attr, 0, sizeof(resmgr_attr));
    resmgr_attr.nparts_max = 2;
    resmgr_attr.msg_max_size = 4096;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    iofunc_attr_init(&ioattr, S_IFCHR | 0666, 0, 0);

    /* Bind handlers */
    io_funcs.read   = safepulse_io_read;
    io_funcs.write  = safepulse_io_write;
    io_funcs.devctl = safepulse_io_devctl;

    resmgr_id = resmgr_attach(dpp, &resmgr_attr, RESMGR_DEVICE_NAME, _FTYPE_ANY, 0,
                              &connect_funcs, &io_funcs, &ioattr);
    if (resmgr_id == -1) {
        fprintf(stderr, "resmgr_attach %s failed: %s\n", RESMGR_DEVICE_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    ctp = dispatch_context_alloc(dpp);
    if (!ctp) {
        fprintf(stderr, "dispatch_context_alloc failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Enforce ASIL-D Real-Time Microkernel Scheduling (SCHED_FIFO / SCHED_SPORADIC) */
    struct sched_param rt_param;
    memset(&rt_param, 0, sizeof(rt_param));

    /* Attempt SCHED_SPORADIC for deterministic CPU budget enforcement & FFI */
    rt_param.sched_priority = 50;             /* High-priority execution (Safety-critical) */
    rt_param.sched_ss_low_priority = 15;      /* Degraded priority if budget exhausted (anti-starvation) */
    rt_param.sched_ss_init_budget.tv_sec = 0;
    rt_param.sched_ss_init_budget.tv_nsec = 3000000; /* 3ms guaranteed budget per 10ms */
    rt_param.sched_ss_repl_period.tv_sec = 0;
    rt_param.sched_ss_repl_period.tv_nsec = 10000000; /* 10ms ISO 26262 FTTI period */
    rt_param.sched_ss_max_repl = 8;

    if (sched_setscheduler(0, SCHED_SPORADIC, &rt_param) == 0) {
        g_active_sched_policy = (uint8_t)SCHED_SPORADIC;
        g_active_sched_prio   = (uint8_t)rt_param.sched_priority;
        printf("[SafePulse] QNX Microkernel Scheduling: SCHED_SPORADIC (Pri: 50/15, Budget: 3ms/10ms, FFI active)\n");
    } else {
        /* Fallback to deterministic real-time SCHED_FIFO */
        memset(&rt_param, 0, sizeof(rt_param));
        rt_param.sched_priority = 50;
        if (sched_setscheduler(0, SCHED_FIFO, &rt_param) == 0) {
            g_active_sched_policy = (uint8_t)SCHED_FIFO;
            g_active_sched_prio   = (uint8_t)rt_param.sched_priority;
            printf("[SafePulse] QNX Microkernel Scheduling: SCHED_FIFO (Real-Time Priority 50 enforced)\n");
        } else {
            struct sched_param cur_p;
            int cur_pol = sched_getscheduler(0);
            g_active_sched_policy = (cur_pol >= 0) ? (uint8_t)cur_pol : 0;
            g_active_sched_prio   = (sched_getparam(0, &cur_p) == 0) ? (uint8_t)cur_p.sched_priority : 10;
            fprintf(stderr, "[SafePulse] Warning: sched_setscheduler failed (%s), running with inherited priority\n",
                    strerror(errno));
        }
    }

    printf("[SafePulse] Device registered successfully at %s\n", RESMGR_DEVICE_NAME);
    printf("[SafePulse] Ready to receive telemetry stream...\n");

    /* 3. Microkernel Event Message Loop */
    while (1) {
        ctp = dispatch_block(ctp);
        if (!ctp) {
            fprintf(stderr, "dispatch_block error: %s\n", strerror(errno));
            break;
        }
        dispatch_handler(ctp);
    }

    return EXIT_SUCCESS;
}
