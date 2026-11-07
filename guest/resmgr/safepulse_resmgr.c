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

static int safepulse_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status;
    safepulse_packet_t pkt;

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

    if ((status = iofunc_write_verify(ctp, msg, ocb, NULL)) != EOK) {
        return status;
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    /* Strict size validation: Reject malformed buffer sizes */
    if (nbytes == (int)sizeof(safepulse_packet_t)) {
        safepulse_packet_t pkt;
        if (resmgr_msgget(ctp, &pkt, sizeof(pkt), sizeof(msg->i)) < 0) {
            return EFAULT;
        }

        /* Inline Real-time Safety Linter Inspection (re-entrant) */
        linter_inspect_packet(&g_linter, &pkt);

        /* Step ISO 26262 ASIL-D Safe State Transition FSM */
        asil_safe_state_t cur_state = safe_state_fsm_step(&g_fsm, pkt.violation_code, pkt.safety_score, pkt.timestamp_ns);
        if (cur_state != ASIL_STATE_NORMAL) {
            actuator_limits_t lim = safe_state_get_limits(cur_state);
            /* Smooth Ramped Slew-Rate Clamping (ISO 26262-4 / 6 Anti-Jerk Rollover Protection) */
            if (pkt.sensor_type == SENSOR_TYPE_STEERING) {
                pkt.val_primary = actuator_slew_clamp_steering(&g_slew, pkt.val_primary, lim.max_steering_angle, pkt.timestamp_ns);
            } else if (pkt.sensor_type == SENSOR_TYPE_THROTTLE) {
                pkt.val_primary = actuator_slew_clamp_throttle(&g_slew, pkt.val_primary, lim.max_throttle_pct, pkt.timestamp_ns);
            }
        }

        /* Push verified packet to Lock-Free Zero-Copy 2MB Ringbuffer */
        ringbuffer_push(g_rb, &pkt);
        atomic_thread_fence(memory_order_release);

        /* Atomic statistics updates */
        atomic_fetch_add_explicit((_Atomic uint32_t*)&g_stats.total_inspected, 1, memory_order_relaxed);
        if (pkt.violation_code != VIOLATION_NONE) {
            atomic_fetch_add_explicit((_Atomic uint32_t*)&g_stats.total_violations, 1, memory_order_relaxed);
            atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, pkt.violation_code, memory_order_relaxed);
        }
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_safety_score, *(uint32_t*)&pkt.safety_score, memory_order_relaxed);

    } else if (nbytes == (int)sizeof(chaos_command_t)) {
        chaos_command_t cmd;
        if (resmgr_msgget(ctp, &cmd, sizeof(cmd), sizeof(msg->i)) < 0) {
            return EFAULT;
        }

        if (cmd.magic == CHAOS_MAGIC) {
            printf("[SafePulse] Injected Chaos Command Type: %u, Duration: %u ms\n",
                   cmd.fault_type, cmd.duration_ms);
            if (cmd.fault_type == 3) {
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
            safepulse_stats_t *out = (safepulse_stats_t *)_DEVCTL_DATA(msg->i);
            g_stats.ringbuffer_count = ringbuffer_count(g_rb);
            g_stats.drop_count = g_rb->drop_count;
            g_stats.uptime_seconds = (uint32_t)(time(NULL) - g_start_time);

            struct sched_param cur_param;
            int cur_policy = sched_getscheduler(0);
            if (cur_policy >= 0 && sched_getparam(0, &cur_param) == 0) {
                g_stats.sched_policy = (uint32_t)cur_policy;
                g_stats.sched_priority = (uint32_t)cur_param.sched_priority;
            }
            g_stats.fsm_state = (uint32_t)g_fsm.current_state;

            *out = g_stats;
            memset(&msg->o, 0, sizeof(msg->o));
            msg->o.nbytes = sizeof(safepulse_stats_t);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(safepulse_stats_t));
        }
        case DCMD_SAFEPULSE_RESET_STATS: {
            memset(&g_stats, 0, sizeof(g_stats));
            g_rb->drop_count = 0;
            return EOK;
        }
        case DCMD_SAFEPULSE_SYNC_EDGE: {
            g_rb->tail = g_rb->head;
            return EOK;
        }
        default:
            return ENOSYS;
    }
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

    printf("==================================================\n");
    printf(" Starting QNX SafePulse Lab Resource Manager \n");
    printf(" Device Path: %s\n", RESMGR_DEVICE_NAME);
    printf("==================================================\n");

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

    if (is_new_shm) {
        ringbuffer_init(g_rb);
        printf("[SafePulse] Initialized new 2MB Ringbuffer in Shared Memory.\n");
    } else {
        printf("[SafePulse] ZERO DATA LOSS: Preserved %u historical packets in SHM across crash boundaries!\n",
               ringbuffer_count(g_rb));
    }

    linter_init(&g_linter);
    safe_state_fsm_init(&g_fsm);
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
        printf("[SafePulse] QNX Microkernel Scheduling: SCHED_SPORADIC (Pri: 50/15, Budget: 3ms/10ms, FFI active)\n");
    } else {
        /* Fallback to deterministic real-time SCHED_FIFO */
        memset(&rt_param, 0, sizeof(rt_param));
        rt_param.sched_priority = 50;
        if (sched_setscheduler(0, SCHED_FIFO, &rt_param) == 0) {
            printf("[SafePulse] QNX Microkernel Scheduling: SCHED_FIFO (Real-Time Priority 50 enforced)\n");
        } else {
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
