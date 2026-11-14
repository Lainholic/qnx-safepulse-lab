#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/iofunc.h>
#include <sys/iomgr.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syspage.h>
#include <sys/slog2.h>
#include <secpol/secpol.h>
#include <stdatomic.h>

#include "safepulse.h"
#include "safepulse_devctl.h"
#include "safepulse_identity.h"
#include "ringbuffer.h"
#include "linter.h"
#include "safe_state_fsm.h"

#define RESMGR_DEVICE_NAME "/dev/safepulse"
#define SHM_NAME           "/safepulse_ringbuf"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          ioattr;
static iofunc_notify_t        g_notify[3];

static ringbuffer_t    *g_rb = NULL;
static linter_state_t   g_linter;
static safe_state_fsm_t g_fsm;
static actuator_slew_state_t g_slew;
static safepulse_stats_t g_stats;
static time_t           g_start_time = 0;
static uint8_t          g_active_sched_policy = 0;
static uint8_t          g_active_sched_prio   = 0;
static int              g_secure_mode = 0;

static slog2_buffer_t   g_slog_buf = NULL;
static uint32_t         g_active_profile = SAFEPULSE_PROFILE_PASSENGER;
static char             g_custom_rules_cache[2048];
static uint32_t         g_custom_rules_crc = 0U;
static asil_safe_state_t g_last_slog_state = ASIL_STATE_NORMAL;
static uint32_t         g_last_slog_viol = VIOLATION_NONE;
static int              g_type_enforce_active = 0;
static uint32_t         g_type_bridge   = SECPOL_INVALID_TYPE;
static uint32_t         g_type_steering = SECPOL_INVALID_TYPE;
static uint32_t         g_type_braking  = SECPOL_INVALID_TYPE;
static uint32_t         g_type_power    = SECPOL_INVALID_TYPE;

static void init_safepulse_slog2(void) {
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffer_set_name = "safepulse_resmgr";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "safety";
    cfg.buffer_config[0].num_pages = 2;
    (void)slog2_register(&cfg, &g_slog_buf, SLOG2_QUIET);
}

static void __attribute__((format(printf, 2, 3)))
sp_slog(uint8_t severity, const char *fmt, ...) {
    if (!g_slog_buf) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vslog2f(g_slog_buf, 0, severity, fmt, ap);
    va_end(ap);
}

static void init_secpol_type_enforcement(void) {
    if (!g_secure_mode) {
        return;
    }
    secpol_file_t *spf = secpol_open(NULL, 0);
    if (!spf) {
        sp_slog(SLOG2_CRITICAL, "secpol_open failed in secure mode; refusing insecure fallback");
        fprintf(stderr, "[FATAL] secpol_open failed in secure mode! Refusing to start without security policy.\n");
        exit(EXIT_FAILURE);
    }
    uint32_t self_tid = secpol_get_type_id(spf, "safepulse_resmgr_t");
    g_type_bridge   = secpol_get_type_id(spf, "safepulse_bridge_t");
    g_type_steering = secpol_get_type_id(spf, "safepulse_steering_t");
    g_type_braking  = secpol_get_type_id(spf, "safepulse_braking_t");
    g_type_power    = secpol_get_type_id(spf, "safepulse_power_t");
    secpol_close(spf);

    if (self_tid != SECPOL_INVALID_TYPE &&
        secpol_type_id() == self_tid &&
        g_type_bridge   != SECPOL_INVALID_TYPE &&
        g_type_steering != SECPOL_INVALID_TYPE &&
        g_type_braking  != SECPOL_INVALID_TYPE &&
        g_type_power    != SECPOL_INVALID_TYPE) {
        g_type_enforce_active = 1;
        printf("[SafePulse] Kernel secpol per-ECU type enforcement ACTIVE (bridge=%u steer=%u brake=%u power=%u)\n",
               g_type_bridge, g_type_steering, g_type_braking, g_type_power);
        sp_slog(SLOG2_INFO, "secpol per-ECU type enforcement active");
    } else {
        sp_slog(SLOG2_CRITICAL, "secpol type IDs incomplete or self-type mismatch in secure mode; aborting");
        fprintf(stderr, "[FATAL] secpol type IDs incomplete or self-type mismatch! Aborting under SAFEPULSE_SECURE=1.\n");
        exit(EXIT_FAILURE);
    }
}

static int query_client_secpol_type(resmgr_context_t *ctp, int ioflag, uint32_t *out_type_id, uid_t *out_euid) {
    if (out_euid) {
        struct _client_info client;
        int status = iofunc_client_info(ctp, ioflag, &client);
        if (status != EOK) {
            return status;
        }
        *out_euid = client.cred.euid;
    }
    if (out_type_id) {
        unsigned tid = SECPOL_INVALID_TYPE;
        struct _client_info ext;
        memset(&ext, 0, sizeof(ext));
        if (ConnectClientInfoExtraArgs(ctp->info.scoid, &ext, 0, NULL, 0, &tid) == 0) {
            *out_type_id = (uint32_t)tid;
        } else {
            *out_type_id = SECPOL_INVALID_TYPE;
        }
    }
    return EOK;
}

static int require_bridge_authority(resmgr_context_t *ctp, int ioflag) {
    if (!g_secure_mode) {
        return EOK;
    }
    uint32_t tid = SECPOL_INVALID_TYPE;
    uid_t uid = (uid_t)-1;
    int status = query_client_secpol_type(ctp, ioflag, &tid, &uid);
    if (status != EOK) {
        return status;
    }
    if (uid != SAFEPULSE_UID_BRIDGE) {
        return EACCES;
    }
    if (g_type_enforce_active && tid != g_type_bridge) {
        sp_slog(SLOG2_WARNING, "Blocked privileged control call from non-bridge secpol type_id=%u", tid);
        return EACCES;
    }
    return EOK;
}

static int verify_writer_channel_authority(resmgr_context_t *ctp, int ioflag, uint16_t sensor_type) {
    if (!g_type_enforce_active) {
        return EOK;
    }
    uint32_t tid = SECPOL_INVALID_TYPE;
    int status = query_client_secpol_type(ctp, ioflag, &tid, NULL);
    if (status != EOK) {
        return status;
    }
    if (tid == g_type_bridge) {
        return EOK;
    }
    bool allowed = false;
    if (tid == g_type_steering && sensor_type == SENSOR_TYPE_STEERING) {
        allowed = true;
    } else if (tid == g_type_braking && sensor_type == SENSOR_TYPE_BRAKE) {
        allowed = true;
    } else if (tid == g_type_power && sensor_type == SENSOR_TYPE_THROTTLE) {
        allowed = true;
    }
    if (!allowed) {
        sp_slog(SLOG2_WARNING, "Blocked cross-ECU spoofing: type_id=%u wrote sensor_type=%u",
                tid, (unsigned)sensor_type);
        return EACCES;
    }
    return EOK;
}

static int load_first_existing_rules_file(linter_state_t *linter, uint64_t *starv_ns, const char *const *paths, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (linter_load_rules_file(linter, starv_ns, paths[i]) == 0) {
            return 0;
        }
    }
    return -1;
}

static int apply_active_rules_profile(uint32_t profile_id) {
    static const char *const passenger_paths[] = {
        "/proc/boot/rules.json",
        "rules.json",
        "../rules.json"
    };
    static const char *const truck_paths[] = {
        "/proc/boot/truck_rules.yaml",
        "examples/custom_rules/truck_rules.yaml",
        "../examples/custom_rules/truck_rules.yaml"
    };

    linter_state_t next_linter;
    uint32_t saved_insp = g_linter.total_inspected;
    uint32_t saved_viol = g_linter.total_violations;
    e2e_channel_state_t saved_e2e[MAX_TRACKED_SENSORS];
    memcpy(saved_e2e, g_linter.e2e_channels, sizeof(saved_e2e));

    linter_init(&next_linter);
    next_linter.total_inspected = saved_insp;
    next_linter.total_violations = saved_viol;
    memcpy(next_linter.e2e_channels, saved_e2e, sizeof(saved_e2e));

    uint64_t next_starv_ns = STARVATION_LIMIT_NS;
    int rc = -1;
    const char *prof_name = "PASSENGER";

    if (profile_id == SAFEPULSE_PROFILE_PASSENGER) {
        prof_name = "PASSENGER (rules.json)";
        rc = load_first_existing_rules_file(&next_linter, &next_starv_ns, passenger_paths,
                                            sizeof(passenger_paths) / sizeof(passenger_paths[0]));
    } else if (profile_id == SAFEPULSE_PROFILE_TRUCK) {
        prof_name = "TRUCK (truck_rules.yaml)";
        rc = load_first_existing_rules_file(&next_linter, &next_starv_ns, truck_paths,
                                            sizeof(truck_paths) / sizeof(truck_paths[0]));
    } else if (profile_id == SAFEPULSE_PROFILE_CUSTOM) {
        prof_name = "CUSTOM";
        rc = linter_load_rules_string(&next_linter, &next_starv_ns, g_custom_rules_cache);
    }

    if (rc != 0) {
        sp_slog(SLOG2_WARNING, "Failed to load vehicle rule profile %u (%s), keeping previous rules",
                (unsigned)profile_id, prof_name);
        return -1;
    }

    g_linter = next_linter;
    g_fsm.starvation_limit_ns = next_starv_ns;
    g_active_profile = profile_id;
    printf("[SafePulse] Loaded dynamic safety profile %u: %s (watchdog=%llu ms, steer=[%.0f,%.0f], brake_max=%.0f)\n",
           (unsigned)profile_id, prof_name,
           (unsigned long long)(g_fsm.starvation_limit_ns / 1000000ULL),
           g_linter.rules[SENSOR_TYPE_STEERING].min_bound,
           g_linter.rules[SENSOR_TYPE_STEERING].max_bound,
           g_linter.rules[SENSOR_TYPE_BRAKE].max_bound);
    sp_slog(SLOG2_INFO, "Loaded rule profile %u (%s), watchdog=%llu ms",
            (unsigned)profile_id, prof_name,
            (unsigned long long)(g_fsm.starvation_limit_ns / 1000000ULL));
    return 0;
}

static uint32_t persist_crc_of(uint32_t state, uint32_t latched, uint64_t latch_time,
                               uint32_t viol, uint32_t profile_id, uint32_t custom_crc) {
    uint32_t crc_payload[7] = {
        state,
        latched,
        (uint32_t)(latch_time & 0xFFFFFFFFU),
        (uint32_t)(latch_time >> 32),
        viol,
        profile_id,
        custom_crc
    };
    return safepulse_crc32((const uint8_t*)crc_payload, sizeof(crc_payload));
}

static void persist_fsm_impl(uint32_t viol_code, bool sync_custom_blob) {
    if (!g_rb) {
        return;
    }
    uint32_t state = (uint32_t)g_fsm.current_state;
    uint32_t latched = g_fsm.is_latched ? 1U : 0U;
    uint64_t t = g_fsm.state_entered_ns;
    uint32_t prof = g_active_profile;
    uint32_t ccrc = (prof == SAFEPULSE_PROFILE_CUSTOM) ? g_custom_rules_crc : 0U;
    uint32_t crc = persist_crc_of(state, latched, t, viol_code, prof, ccrc);

    uint32_t seq = atomic_load_explicit(&g_rb->persistent_seq, memory_order_relaxed);
    if ((seq & 1U) != 0U) {
        seq++;
    }
    atomic_store_explicit(&g_rb->persistent_seq, seq + 1U, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);

    atomic_store_explicit(&g_rb->persistent_fsm_state, state, memory_order_relaxed);
    atomic_store_explicit(&g_rb->persistent_fsm_latched, latched, memory_order_relaxed);
    atomic_store_explicit(&g_rb->persistent_latch_time, t, memory_order_relaxed);
    atomic_store_explicit(&g_rb->persistent_violation_code, viol_code, memory_order_relaxed);
    atomic_store_explicit(&g_rb->persistent_profile_id, prof, memory_order_relaxed);
    if (sync_custom_blob) {
        memcpy(g_rb->persistent_custom_rules, g_custom_rules_cache, sizeof(g_rb->persistent_custom_rules));
    }
    atomic_store_explicit(&g_rb->persistent_crc32, crc, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);

    atomic_store_explicit(&g_rb->persistent_seq, seq + 2U, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
}

static void persist_fsm_to_shm(uint32_t viol_code) {
    persist_fsm_impl(viol_code, false);
}

static void persist_custom_rules_and_fsm_to_shm(uint32_t viol_code) {
    persist_fsm_impl(viol_code, true);
}

static void stamp_latched_violation(safepulse_packet_t *pkt) {
    if (g_fsm.current_state != ASIL_STATE_EMERGENCY_STOP) {
        return;
    }
    if (g_fsm.starvation_estop) {
        if (pkt->violation_code == VIOLATION_NONE) {
            pkt->violation_code = VIOLATION_STARVATION;
        }
        return;
    }
    if (pkt->violation_code == VIOLATION_NONE && g_stats.last_violation_code != VIOLATION_NONE) {
        pkt->violation_code = g_stats.last_violation_code;
    }
}

static void check_and_persist_starvation_at(uint64_t now_ns) {
    bool was_estop = (g_fsm.current_state == ASIL_STATE_EMERGENCY_STOP);
    safe_state_fsm_check_starvation(&g_fsm, now_ns);
    if (!was_estop && g_fsm.current_state == ASIL_STATE_EMERGENCY_STOP && g_fsm.starvation_estop) {
        atomic_fetch_add_explicit((_Atomic uint32_t*)&g_stats.total_violations, 1, memory_order_relaxed);
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, VIOLATION_STARVATION, memory_order_relaxed);
        g_stats.fsm_state = (uint32_t)ASIL_STATE_EMERGENCY_STOP;
        persist_fsm_to_shm(VIOLATION_STARVATION);
        sp_slog(SLOG2_CRITICAL, "Starvation Watchdog E-STOP: source=%s starved_sensor=%u",
                (g_fsm.starved_sensor_type > 0U) ? "SINGLE_ECU" : "GLOBAL_BUS",
                (unsigned)g_fsm.starved_sensor_type);
        g_last_slog_state = ASIL_STATE_EMERGENCY_STOP;
        g_last_slog_viol = VIOLATION_STARVATION;
    }
}

static int safepulse_io_notify(resmgr_context_t *ctp, io_notify_t *msg, RESMGR_OCB_T *ocb) {
    (void)ocb;
    int trig = _NOTIFY_COND_OUTPUT;
    if (g_rb && ringbuffer_count(g_rb) > 0U) {
        trig |= _NOTIFY_COND_INPUT;
    }
    return iofunc_notify(ctp, msg, g_notify, trig, NULL, NULL);
}

static int safepulse_io_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb) {
    iofunc_notify_remove(ctp, g_notify);
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

static int safepulse_io_open(resmgr_context_t *ctp, io_open_t *msg,
                             RESMGR_HANDLE_T *handle, void *extra) {
    if (g_secure_mode && (msg->connect.ioflag & _IO_FLAG_WR)) {
        uint32_t tid = SECPOL_INVALID_TYPE;
        uid_t uid = (uid_t)-1;
        int status = query_client_secpol_type(ctp, msg->connect.ioflag,
                                              g_type_enforce_active ? &tid : NULL, &uid);
        if (status != EOK) return status;
        if (uid != SAFEPULSE_UID_BRIDGE && uid != SAFEPULSE_UID_STEERING &&
            uid != SAFEPULSE_UID_BRAKING && uid != SAFEPULSE_UID_POWER) {
            return EACCES;
        }
        if (g_type_enforce_active &&
            tid != g_type_bridge && tid != g_type_steering &&
            tid != g_type_braking && tid != g_type_power) {
            return EACCES;
        }
    }
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int safepulse_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status;
    safepulse_packet_t pkt;

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;
    check_and_persist_starvation_at(now_ns);

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
        /* E-STOP: outgoing packets keep the latched root cause (watchdog or crash) */
        {
            uint32_t before = pkt.violation_code;
            stamp_latched_violation(&pkt);
            if (pkt.violation_code != before) {
                pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
            }
        }
        MsgReply(ctp->rcvid, sizeof(pkt), &pkt, sizeof(pkt));
        return _RESMGR_NOREPLY;
    } else {
        /* No new packet in ringbuffer: return 0 bytes */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
}

static int execute_chaos_command(const chaos_command_t *cmd, uint64_t now_ns, bool strict_errors) {
    if (!cmd || cmd->magic != CHAOS_MAGIC) {
        return strict_errors ? EINVAL : EOK;
    }

    printf("[SafePulse] Injected Chaos Command Type: %u, Duration: %u ms\n",
           cmd->fault_type, cmd->duration_ms);
    if (cmd->fault_type == 0 && g_fsm.current_state == ASIL_STATE_EMERGENCY_STOP &&
        g_slew.last_clamped_speed > 0.05) {
        /* Reset interlock: the latch stays until QNX has braked the vehicle to a full stop. */
        printf("[SafePulse] Reset rejected: vehicle still braking (%.1f km/h)\n", g_slew.last_clamped_speed);
        return strict_errors ? EBUSY : EOK;
    } else if (cmd->fault_type == 0) {
        printf("[SafePulse] Operator Command: Clearing Fault Latch -> Restoring ASIL_STATE_NORMAL\n");
        safe_state_fsm_init(&g_fsm, now_ns);
        linter_init(&g_linter);
        (void)apply_active_rules_profile(g_active_profile);
        g_last_slog_state = ASIL_STATE_NORMAL;
        g_last_slog_viol = VIOLATION_NONE;
        sp_slog(SLOG2_NOTICE, "Operator Reset cleared latch -> NORMAL (profile=%u)",
                (unsigned)g_active_profile);
        {
            /* Restart from rest: keep the channel clocks so steering, throttle and speed ramp up from 0. */
            actuator_slew_state_t prev = g_slew;
            actuator_slew_init(&g_slew);
            g_slew.last_steer_timestamp_ns = prev.last_steer_timestamp_ns;
            g_slew.last_throttle_timestamp_ns = prev.last_throttle_timestamp_ns;
            g_slew.last_brake_timestamp_ns = prev.last_brake_timestamp_ns;
            g_slew.last_speed_timestamp_ns = prev.last_speed_timestamp_ns;
        }
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, 0, memory_order_relaxed);
        persist_fsm_to_shm(0);
    } else if (cmd->fault_type == 3 || cmd->fault_type == 6 ||
               cmd->fault_type == 7 || cmd->fault_type == 8) {
#ifdef SAFEPULSE_FAULT_INJECTION
        uint32_t crash_code = VIOLATION_CRASH_SIGSEGV;
        const char *crash_name = "SIGSEGV - Null Pointer Dereference";
        if (cmd->fault_type == 6) {
            crash_code = VIOLATION_CRASH_SIGFPE;
            crash_name = "SIGFPE - Arithmetic Divide by Zero";
        } else if (cmd->fault_type == 7) {
            crash_code = VIOLATION_CRASH_SIGABRT;
            crash_name = "SIGABRT - Critical Assertion Abort";
        } else if (cmd->fault_type == 8) {
            crash_code = VIOLATION_CRASH_SIGILL;
            crash_name = "SIGILL - Illegal Opcode Execution";
        }
        printf("[SafePulse] Latching EMERGENCY_STOP + 0x%04X into persistent SHM before %s...\n",
               crash_code, crash_name);
        safe_state_fsm_force_estop(&g_fsm, now_ns);
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, crash_code, memory_order_relaxed);
        persist_fsm_to_shm(crash_code);
        sp_slog(SLOG2_CRITICAL, "Pre-crash E-STOP latch committed before %s (0x%04X)",
                crash_name, crash_code);
        fflush(stdout);
        if (cmd->fault_type == 3) {
            printf("[SafePulse] Triggering Induced Fault Crash (SIGSEGV - Null Pointer Dereference)...\n");
            fflush(stdout);
            volatile int *null_ptr = NULL;
            *null_ptr = 0xDEAD; /* Induce SIGSEGV to verify self-healing recovery */
        } else if (cmd->fault_type == 6) {
            printf("[SafePulse] Triggering Induced Fault Crash (SIGFPE - Arithmetic Divide by Zero)...\n");
            fflush(stdout);
            volatile int zero = 0;
            volatile int res = 100 / zero;
            (void)res;
        } else if (cmd->fault_type == 7) {
            printf("[SafePulse] Triggering Induced Fault Crash (SIGABRT - Critical Assertion Abort)...\n");
            fflush(stdout);
            abort();
        } else if (cmd->fault_type == 8) {
            printf("[SafePulse] Triggering Induced Fault Crash (SIGILL - Illegal Opcode Execution)...\n");
            fflush(stdout);
            __builtin_trap();
        }
#else
        printf("[SafePulse] Fault injection disabled in production build (type=%u ignored)\n", cmd->fault_type);
        sp_slog(SLOG2_WARNING, "Fault injection rejected: SAFEPULSE_FAULT_INJECTION not defined");
        return strict_errors ? EPERM : EOK;
#endif
    }
    return EOK;
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
        safepulse_packet_t pkt;
        if (resmgr_msgget(ctp, &pkt, sizeof(pkt), sizeof(msg->i)) < 0) {
            return EFAULT;
        }
        status = verify_writer_channel_authority(ctp, ocb->ioflag, pkt.sensor_type);
        if (status != EOK) {
            return status;
        }
        check_and_persist_starvation_at(now_ns);
        const uint64_t inspect_start_cc = ClockCycles();
        if (g_fsm.last_packet_timestamp_ns != 0 && now_ns > g_fsm.last_packet_timestamp_ns) {
            /* Longest silence between sensor packets (same clock as the starvation watchdog). */
            uint64_t gap = now_ns - g_fsm.last_packet_timestamp_ns;
            if (gap > 0xFFFFFFFFULL) gap = 0xFFFFFFFFULL;
            if ((uint32_t)gap > g_stats.max_gap_ns) g_stats.max_gap_ns = (uint32_t)gap;
        }

        /* Inline Real-time Safety Linter Inspection (re-entrant) */
        linter_inspect_packet(&g_linter, &pkt);

        /* Step ISO 26262 ASIL-D Safe State Transition FSM */
        asil_safe_state_t cur_state = safe_state_fsm_step(&g_fsm, pkt.violation_code, pkt.safety_score, pkt.timestamp_ns);
        /* Anchor global & per-channel starvation heartbeat strictly to QNX kernel monotonic clock domain */
        safe_state_fsm_record_channel_rx(&g_fsm, pkt.sensor_type, now_ns);
        if (cur_state != g_last_slog_state ||
            (pkt.violation_code != VIOLATION_NONE && pkt.violation_code != g_last_slog_viol)) {
            sp_slog((cur_state == ASIL_STATE_EMERGENCY_STOP) ? SLOG2_CRITICAL : SLOG2_WARNING,
                    "FSM=%s viol=0x%04X sensor=%u score=%.1f",
                    safe_state_to_string(cur_state), pkt.violation_code,
                    (unsigned)pkt.sensor_type, (double)pkt.safety_score);
            g_last_slog_state = cur_state;
            if (pkt.violation_code != VIOLATION_NONE) {
                g_last_slog_viol = pkt.violation_code;
            }
        }
        stamp_latched_violation(&pkt);
        {
            uint32_t persist_viol = 0;
            if (g_fsm.is_latched || cur_state != ASIL_STATE_NORMAL) {
                persist_viol = (pkt.violation_code != VIOLATION_NONE)
                    ? pkt.violation_code
                    : g_stats.last_violation_code;
            }
            persist_fsm_to_shm(persist_viol);
        }

        {
            actuator_limits_t lim = safe_state_get_limits(cur_state);
            /* Apply QNX actuator limits in every state so limited driving can
               ramp from the last live values. Speed is not zeroed on E-STOP
               so the host can hold the last measured speed. */
            if (pkt.sensor_type == SENSOR_TYPE_STEERING) {
                double prev_steer = g_slew.last_clamped_steering;
                uint64_t prev_steer_ts = g_slew.last_steer_timestamp_ns;
                pkt.val_primary = actuator_slew_clamp_steering(&g_slew, pkt.val_primary, lim.max_steering_angle, lim.max_steering_slew_deg_s, pkt.timestamp_ns);
                if (cur_state == ASIL_STATE_EMERGENCY_STOP) {
                    /* In E-STOP the ECU's rate is ignored: report how fast the wheel really moves (0 once centered). */
                    double rate = 0.0;
                    if (prev_steer_ts != 0 && pkt.timestamp_ns > prev_steer_ts) {
                        rate = (pkt.val_primary - prev_steer) / ((double)(pkt.timestamp_ns - prev_steer_ts) / 1000000000.0);
                    }
                    pkt.val_secondary = rate;
                }
                if (pkt.val_secondary > lim.max_steering_slew_deg_s) {
                    pkt.val_secondary = lim.max_steering_slew_deg_s;
                } else if (pkt.val_secondary < -lim.max_steering_slew_deg_s) {
                    pkt.val_secondary = -lim.max_steering_slew_deg_s;
                }
            } else if (pkt.sensor_type == SENSOR_TYPE_THROTTLE) {
                pkt.val_primary = actuator_slew_clamp_throttle(&g_slew, pkt.val_primary, lim.max_throttle_pct, pkt.timestamp_ns);
                if (cur_state != ASIL_STATE_EMERGENCY_STOP) {
                    pkt.val_secondary = actuator_slew_clamp_speed(&g_slew, pkt.val_secondary, lim.max_speed_kmh, pkt.timestamp_ns);
                } else {
                    /* QNX owns the stop: ignore the ECU's speed and brake down to 0. */
                    pkt.val_secondary = actuator_brake_speed_to_stop(&g_slew, ESTOP_BRAKE_DECEL_KMH_S, pkt.val_secondary, pkt.timestamp_ns);
                }
            } else if (pkt.sensor_type == SENSOR_TYPE_BRAKE) {
                /* ASIL-D Safe Braking: Enforce minimum deceleration brake pressure (e.g. 100 bar on E-STOP) and reject NaN/Inf */
                if (!isfinite(pkt.val_primary) || pkt.val_primary < lim.min_brake_bar) {
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
        {
            int qcnt = (int)ringbuffer_count(g_rb);
            if (IOFUNC_NOTIFY_INPUT_CHECK(g_notify, qcnt, 1)) {
                iofunc_notify_trigger(g_notify, qcnt, IOFUNC_NOTIFY_INPUT);
            }
        }
        {
            /* Inline inspection cost measured with the CPU cycle counter. */
            const uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
            const uint64_t cc = ClockCycles() - inspect_start_cc;
            uint64_t ns = cps ? (uint64_t)((double)cc * 1e9 / (double)cps) : 0;
            if (ns > 0xFFFFFFFFULL) ns = 0xFFFFFFFFULL;
            if ((uint32_t)ns > g_stats.max_inspect_ns) g_stats.max_inspect_ns = (uint32_t)ns;
        }

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
        if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
            return status;
        }
        chaos_command_t cmd;
        if (resmgr_msgget(ctp, &cmd, sizeof(cmd), sizeof(msg->i)) < 0) {
            return EFAULT;
        }
        (void)execute_chaos_command(&cmd, now_ns, false);
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
        case DCMD_SAFEPULSE_GET_PID: {
            if (!(ocb->ioflag & _IO_FLAG_RD)) return EPERM;
            if (msg->i.nbytes < sizeof(uint32_t)) return EINVAL;
            uint32_t *out = (uint32_t *)_DEVCTL_DATA(msg->i);
            *out = (uint32_t)getpid();
            memset(&msg->o, 0, sizeof(msg->o));
            msg->o.nbytes = sizeof(*out);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*out));
        }
        case DCMD_SAFEPULSE_GET_STATS: {
            if (!(ocb->ioflag & _IO_FLAG_RD)) {
                return EPERM;
            }
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;
            check_and_persist_starvation_at(now_ns);

            safepulse_stats_t *out = (safepulse_stats_t *)_DEVCTL_DATA(msg->i);
            g_stats.ringbuffer_count = ringbuffer_count(g_rb);
            g_stats.drop_count = atomic_load_explicit(&g_rb->drop_count, memory_order_acquire);
            g_stats.uptime_seconds = (uint32_t)(time(NULL) - g_start_time);

            g_stats.sched_policy = (uint32_t)g_active_sched_policy;
            g_stats.sched_priority = (uint32_t)g_active_sched_prio;
            g_stats.fsm_state = (uint32_t)g_fsm.current_state;
            g_stats.active_profile = g_active_profile;
            g_stats.starved_sensor = (uint32_t)g_fsm.starved_sensor_type;
            g_stats.starvation_limit_ms = (uint32_t)((g_fsm.starvation_limit_ns > 0ULL
                ? g_fsm.starvation_limit_ns
                : STARVATION_LIMIT_NS) / 1000000ULL);

            *out = g_stats;
            memset(&msg->o, 0, sizeof(msg->o));
            msg->o.nbytes = sizeof(safepulse_stats_t);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(safepulse_stats_t));
        }
        case DCMD_SAFEPULSE_RESET_STATS: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            memset(&g_stats, 0, sizeof(g_stats));
            atomic_store_explicit(&g_rb->drop_count, 0, memory_order_release);
            return EOK;
        }
        case DCMD_SAFEPULSE_INJECT_CHAOS: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            if (msg->i.nbytes < (int)sizeof(chaos_command_t)) {
                return EINVAL;
            }
            chaos_command_t cmd;
            memcpy(&cmd, _DEVCTL_DATA(msg->i), sizeof(cmd));
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            uint64_t now_ns = ((uint64_t)ts_now.tv_sec * 1000000000ULL) + (uint64_t)ts_now.tv_nsec;
            return execute_chaos_command(&cmd, now_ns, true);
        }
        case DCMD_SAFEPULSE_RESET_LATENCY: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            g_stats.max_inspect_ns = 0;
            g_stats.max_gap_ns = 0;
            return EOK;
        }
        case DCMD_SAFEPULSE_SYNC_EDGE: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            uint32_t h = atomic_load_explicit(&g_rb->head, memory_order_acquire);
            atomic_store_explicit(&g_rb->tail, h, memory_order_release);
            return EOK;
        }
        case DCMD_SAFEPULSE_LOAD_PROFILE: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            if (msg->i.nbytes < (int)sizeof(uint32_t)) {
                return EINVAL;
            }
            uint32_t prof = 0;
            memcpy(&prof, _DEVCTL_DATA(msg->i), sizeof(prof));
            if (prof != SAFEPULSE_PROFILE_PASSENGER && prof != SAFEPULSE_PROFILE_TRUCK) {
                return EINVAL;
            }
            if (apply_active_rules_profile(prof) != 0) {
                return ENOENT;
            }
            {
                uint32_t persist_viol = (g_fsm.is_latched || g_fsm.current_state != ASIL_STATE_NORMAL)
                    ? g_stats.last_violation_code
                    : 0U;
                persist_fsm_to_shm(persist_viol);
            }
            return EOK;
        }
        case DCMD_SAFEPULSE_LOAD_RULES: {
            if (!(ocb->ioflag & _IO_FLAG_WR)) {
                return EPERM;
            }
            if ((status = require_bridge_authority(ctp, ocb->ioflag)) != EOK) {
                return status;
            }
            if (msg->i.nbytes < (int)sizeof(safepulse_rules_blob_t)) {
                return EINVAL;
            }
            const safepulse_rules_blob_t *blob = (const safepulse_rules_blob_t *)_DEVCTL_DATA(msg->i);
            char prev_cache[2048];
            memcpy(prev_cache, g_custom_rules_cache, sizeof(prev_cache));
            memcpy(g_custom_rules_cache, blob->payload, sizeof(g_custom_rules_cache) - 1U);
            g_custom_rules_cache[sizeof(g_custom_rules_cache) - 1U] = '\0';
            if (apply_active_rules_profile(SAFEPULSE_PROFILE_CUSTOM) != 0) {
                memcpy(g_custom_rules_cache, prev_cache, sizeof(g_custom_rules_cache));
                return EINVAL;
            }
            g_custom_rules_crc = safepulse_crc32((const uint8_t *)g_custom_rules_cache, sizeof(g_custom_rules_cache));
            {
                uint32_t persist_viol = (g_fsm.is_latched || g_fsm.current_state != ASIL_STATE_NORMAL)
                    ? g_stats.last_violation_code
                    : 0U;
                persist_custom_rules_and_fsm_to_shm(persist_viol);
            }
            return EOK;
        }
        default:
            return ENOSYS;
    }
}

/* =========================================================================
 * ISO 26262 ASIL-D Power-On Built-In Self-Test (BIST / POST)
 * Runs 10 comprehensive unit tests on safety invariants before /dev/safepulse
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
    printf("  [BIST 1/10] Linter State Initialization: PASS\n");

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
    printf("  [BIST 2/10] Nominal Sensor Validation & CRC Verification: PASS\n");

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
    printf("  [BIST 3/10] Rule 1 Physical Bounds (0x1001) Enforcement: PASS\n");

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
    printf("  [BIST 4/10] Rule 5 Multi-Sensor Plausibility (0x1005) Detection: PASS\n");

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
    printf("  [BIST 5/10] Rule 4 Hardware CRC-32 Corruption (0x1004) Trap: PASS\n");

    /* BIST-6: Dual-Rail FSM Initialization & Normal Step */
    safe_state_fsm_init(&test_fsm, t0);
    if (test_fsm.current_state != ASIL_STATE_NORMAL || test_fsm.shadow_state != (asil_safe_state_t)(~ASIL_STATE_NORMAL)) {
        fprintf(stderr, "[BIST FAIL] FSM dual-rail shadow register inverted mismatch!\n");
        return -6;
    }
    printf("  [BIST 6/10] Dual-Rail SEU Inverted Shadow State Redundancy: PASS\n");

    /* BIST-7: FSM Upward Escalation & Latching Guarantee */
    safe_state_fsm_step(&test_fsm, VIOLATION_CRC_CORRUPTED, 0.0f, t0 + 1000000ULL);
    if (test_fsm.current_state != ASIL_STATE_EMERGENCY_STOP || !test_fsm.is_latched) {
        fprintf(stderr, "[BIST FAIL] Catastrophic fault did not escalate to E-STOP latch!\n");
        return -7;
    }
    printf("  [BIST 7/10] Unconditional ASIL-D Upward Escalation to E-STOP Latch: PASS\n");

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
    printf("  [BIST 8/10] AUTOSAR E2E Profile 1 Inline Verification (Normal/Bitflip/Skip/Repeat): PASS\n");

    /* BIST-9: IEEE-754 NaN/Inf Trap & Per-ECU Starvation Watchdog */
    linter_init(&test_linter);
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 15U;
    pkt.timestamp_ns = t0;
    pkt.val_primary = 5.0;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_NONE) return -12;

    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 16U;
    pkt.timestamp_ns = t0 + 20000000ULL;
    pkt.val_primary = NAN;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_OUT_OF_BOUNDS) {
        fprintf(stderr, "[BIST FAIL] NaN sample not trapped as 0x1001!\n");
        return -12;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SENSOR_TYPE_STEERING;
    pkt.seq_num = 17U;
    pkt.timestamp_ns = t0 + 40000000ULL;
    pkt.val_primary = 6.0;
    pkt.crc32 = safepulse_crc32((const uint8_t *)&pkt, offsetof(safepulse_packet_t, crc32));
    linter_inspect_packet(&test_linter, &pkt);
    if (pkt.violation_code != VIOLATION_NONE) {
        fprintf(stderr, "[BIST FAIL] NaN poisoned jerk tracking state!\n");
        return -13;
    }

    safe_state_fsm_init(&test_fsm, t0);
    safe_state_fsm_record_channel_rx(&test_fsm, SENSOR_TYPE_STEERING, t0);
    safe_state_fsm_record_channel_rx(&test_fsm, SENSOR_TYPE_STEERING, t0 + 20000000ULL);
    safe_state_fsm_record_channel_rx(&test_fsm, SENSOR_TYPE_BRAKE, t0 + 130000000ULL);
    safe_state_fsm_step(&test_fsm, VIOLATION_NONE, 100.0f, t0 + 130000000ULL);
    safe_state_fsm_check_starvation(&test_fsm, t0 + 150000000ULL);
    if (test_fsm.current_state != ASIL_STATE_EMERGENCY_STOP ||
        !test_fsm.starvation_estop ||
        test_fsm.starved_sensor_type != SENSOR_TYPE_STEERING) {
        fprintf(stderr, "[BIST FAIL] Per-ECU Steering starvation watchdog did not trip! state=%d starved=%u\n",
                (int)test_fsm.current_state, (unsigned)test_fsm.starved_sensor_type);
        return -14;
    }
    printf("  [BIST 9/10] IEEE-754 NaN/Inf Trap & Per-ECU Starvation Watchdog: PASS\n");

    /* BIST-10: Dynamic JSON & YAML Rule Parser Self-Test */
    {
        uint64_t parsed_starv = 0ULL;
        const char *bist_json =
            "{\"watchdog\":{\"starvation_limit_ms\":75},"
            "\"sensors\":[{\"type\":1,\"min_val\":-360,\"max_val\":360,\"max_jerk\":400,\"deadline_ms\":80}]}";
        const char *bist_yaml =
            "starvation_limit_ms: 90\n"
            "- type: 2\n"
            "  max_val: 240.0\n"
            "  period_ms: 20\n"
            "  deadline_tolerance_ms: 60\n";
        linter_init(&test_linter);
        if (linter_load_rules_string(&test_linter, &parsed_starv, bist_json) != 0 ||
            parsed_starv != 75000000ULL ||
            fabs(test_linter.rules[SENSOR_TYPE_STEERING].max_bound - 360.0) > 0.01) {
            fprintf(stderr, "[BIST FAIL] Dynamic JSON rule parser self-test failed!\n");
            return -15;
        }
        if (linter_load_rules_string(&test_linter, &parsed_starv, bist_yaml) != 0 ||
            parsed_starv != 90000000ULL ||
            fabs(test_linter.rules[SENSOR_TYPE_BRAKE].max_bound - 240.0) > 0.01 ||
            test_linter.rules[SENSOR_TYPE_BRAKE].deadline_ns != 80000000ULL) {
            fprintf(stderr, "[BIST FAIL] Dynamic YAML rule parser self-test failed!\n");
            return -16;
        }
    }
    printf("  [BIST 10/10] Dynamic JSON & YAML Rule Parser Self-Test: PASS\n");

    printf("[BIST] ALL 10 ASIL-D POWER-ON SELF-TESTS PASSED (0 Latent Faults Detected).\n\n");
    return 0;
}

int main(int argc, char **argv) {
    dispatch_t           *dpp;
    dispatch_context_t   *ctp;
    resmgr_attr_t         resmgr_attr;
    int                   resmgr_id;
    int                   shm_fd;
    int                   is_new_shm = 0;
    int                   shm_corrupted_recover_estop = 0;

    (void)argc;
    (void)argv;

    g_start_time = time(NULL);
    g_secure_mode = getenv("SAFEPULSE_SECURE") != NULL &&
                    strcmp(getenv("SAFEPULSE_SECURE"), "1") == 0;

    /* Disable blocking disk core dumps; ASIL-D Blackbox Flight Recorder resides in zero-copy POSIX SHM */
    struct rlimit rl_core = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rl_core);
    init_safepulse_slog2();
    init_secpol_type_enforcement();

    printf("==================================================\n");
    printf(" Starting QNX SafePulse Lab Resource Manager \n");
    printf(" Device Path: %s\n", RESMGR_DEVICE_NAME);
    printf("==================================================\n");

    /* 0. Execute ISO 26262 ASIL-D Built-In Self-Test (BIST) before exposing interface */
    if (safepulse_run_bist() != 0) {
        sp_slog(SLOG2_CRITICAL, "SafePulse BIST self-test failed; refusing to attach %s", RESMGR_DEVICE_NAME);
        fprintf(stderr, "[FATAL] SafePulse BIST self-test failed! Refusing to register %s.\n", RESMGR_DEVICE_NAME);
        return EXIT_FAILURE;
    }
    sp_slog(SLOG2_INFO, "ASIL-D BIST 10/10 passed; initializing %s", RESMGR_DEVICE_NAME);

    /* 1. Attach/Create Persistent Shared Memory (Fault-Isolation Survival) */
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shm_fd >= 0) {
        is_new_shm = 1;
        if (ftruncate(shm_fd, sizeof(ringbuffer_t)) != 0) {
            perror("ftruncate shm");
            return EXIT_FAILURE;
        }
    } else if (errno == EEXIST) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
        if (shm_fd < 0) {
            perror("shm_open existing");
            return EXIT_FAILURE;
        }
        struct stat st_shm;
        memset(&st_shm, 0, sizeof(st_shm));
        if (fstat(shm_fd, &st_shm) != 0) {
            perror("fstat existing shm");
            return EXIT_FAILURE;
        }
        if ((size_t)st_shm.st_size < sizeof(ringbuffer_t)) {
            fprintf(stderr, "[SafePulse] Existing SHM file truncated/corrupted (size=%lld, expected=%zu). Re-truncating to ASIL EMERGENCY_STOP.\n",
                    (long long)st_shm.st_size, sizeof(ringbuffer_t));
            if (ftruncate(shm_fd, sizeof(ringbuffer_t)) != 0) {
                perror("ftruncate existing shm");
                return EXIT_FAILURE;
            }
            shm_corrupted_recover_estop = 1;
        } else {
            printf("[SafePulse] Reconnected to existing SHM buffer (size=%lld, Recovered from crash!)\n",
                   (long long)st_shm.st_size);
        }
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

    uint32_t restored_violation = 0;
    uint32_t target_profile = SAFEPULSE_PROFILE_PASSENGER;
    if (is_new_shm) {
        ringbuffer_init(g_rb);
        safe_state_fsm_init(&g_fsm, boot_time_ns);
        persist_fsm_to_shm(0);
        printf("[SafePulse] Initialized new 2MB Ringbuffer & Nominal FSM in Shared Memory.\n");
    } else if (shm_corrupted_recover_estop) {
        /* Truncated/corrupted SHM recovery: re-initialize buffer but force ASIL-D EMERGENCY_STOP */
        ringbuffer_init(g_rb);
        safe_state_fsm_init(&g_fsm, boot_time_ns);
        safe_state_fsm_force_estop(&g_fsm, boot_time_ns);
        restored_violation = VIOLATION_CRC_CORRUPTED;
        persist_fsm_to_shm(restored_violation);
        printf("[SafePulse] Truncated/corrupted SHM re-initialized: ASIL-D Fail-Safe forced EMERGENCY_STOP (0x%04X).\n",
               restored_violation);
        sp_slog(SLOG2_CRITICAL, "Truncated SHM re-initialized: forced EMERGENCY_STOP (0x%04X)", restored_violation);
    } else {
        uint32_t seq1         = atomic_load_explicit(&g_rb->persistent_seq, memory_order_acquire);
        atomic_thread_fence(memory_order_acquire);
        uint32_t prev_state   = atomic_load_explicit(&g_rb->persistent_fsm_state, memory_order_relaxed);
        uint32_t prev_latched = atomic_load_explicit(&g_rb->persistent_fsm_latched, memory_order_relaxed);
        uint64_t prev_time    = atomic_load_explicit(&g_rb->persistent_latch_time, memory_order_relaxed);
        uint32_t prev_viol    = atomic_load_explicit(&g_rb->persistent_violation_code, memory_order_relaxed);
        uint32_t prev_profile = atomic_load_explicit(&g_rb->persistent_profile_id, memory_order_relaxed);
        char     prev_custom[2048];
        uint32_t prev_custom_crc = 0U;
        if (prev_profile == SAFEPULSE_PROFILE_CUSTOM) {
            memcpy(prev_custom, g_rb->persistent_custom_rules, sizeof(prev_custom));
            prev_custom[sizeof(prev_custom) - 1U] = '\0';
            prev_custom_crc = safepulse_crc32((const uint8_t *)prev_custom, sizeof(prev_custom));
        } else {
            memset(prev_custom, 0, sizeof(prev_custom));
        }
        uint32_t stored_crc   = atomic_load_explicit(&g_rb->persistent_crc32, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t seq2         = atomic_load_explicit(&g_rb->persistent_seq, memory_order_acquire);
        uint32_t expected_crc = persist_crc_of(prev_state, prev_latched, prev_time,
                                               prev_viol, prev_profile, prev_custom_crc);

        if ((seq1 & 1U) != 0U || seq1 != seq2 || stored_crc != expected_crc ||
            prev_state > (uint32_t)ASIL_STATE_EMERGENCY_STOP ||
            prev_profile > SAFEPULSE_PROFILE_CUSTOM) {
            /* Fail-Safe Recovery: Torn write or CRC/bounds corruption escalates to EMERGENCY_STOP */
            printf("[SafePulse] Warning: Persistent SHM state invalid (seq=%u/%u, crc=0x%08X/0x%08X)! Fail-Safe escalation to EMERGENCY_STOP.\n",
                   seq1, seq2, stored_crc, expected_crc);
            safe_state_fsm_force_estop(&g_fsm, boot_time_ns);
            restored_violation = (prev_viol != 0U) ? prev_viol : VIOLATION_CRC_CORRUPTED;
            persist_fsm_to_shm(restored_violation);
        } else {
            target_profile = prev_profile;
            if (prev_profile == SAFEPULSE_PROFILE_CUSTOM) {
                memcpy(g_custom_rules_cache, prev_custom, sizeof(g_custom_rules_cache));
                g_custom_rules_crc = prev_custom_crc;
            }
            if (prev_latched || prev_state != (uint32_t)ASIL_STATE_NORMAL) {
                safe_state_fsm_restore(&g_fsm, (asil_safe_state_t)prev_state, prev_time, prev_latched != 0U);
                if (prev_viol == VIOLATION_STARVATION && prev_state == (uint32_t)ASIL_STATE_EMERGENCY_STOP) {
                    g_fsm.starvation_estop = true;
                }
                restored_violation = prev_viol;
                printf("[SafePulse] Restored persistent FSM state %u with violation 0x%04X (profile=%u) from persistent SHM.\n",
                       prev_state, prev_viol, prev_profile);
                sp_slog(SLOG2_WARNING, "Restored persistent FSM=%u viol=0x%04X profile=%u across respawn",
                        prev_state, prev_viol, prev_profile);
            } else {
                safe_state_fsm_init(&g_fsm, boot_time_ns);
            }
        }
        uint32_t total_pushed  = atomic_load_explicit(&g_rb->total_pushed, memory_order_acquire);
        uint32_t retained_hist = (total_pushed < RINGBUFFER_CAPACITY_PACKETS) ? total_pushed : RINGBUFFER_CAPACITY_PACKETS;
        uint32_t unread_queued = ringbuffer_count(g_rb);
        if (unread_queued > RINGBUFFER_CAPACITY_PACKETS) {
            fprintf(stderr, "[SafePulse] Warning: Ringbuffer unread index corrupted (%u > %u). Resetting buffer indices.\n",
                    unread_queued, RINGBUFFER_CAPACITY_PACKETS);
            atomic_store_explicit(&g_rb->tail, atomic_load_explicit(&g_rb->head, memory_order_relaxed), memory_order_release);
            unread_queued = 0;
        }
        uint32_t dropped_cnt   = atomic_load_explicit(&g_rb->drop_count, memory_order_acquire);
        printf("[SafePulse] SHM Ringbuffer Intact: Retained %u historical packets in 2MB SHM (%u unread, %u dropped).\n",
               retained_hist, unread_queued, dropped_cnt);
    }

    linter_init(&g_linter);
    if (apply_active_rules_profile(target_profile) != 0 && target_profile != SAFEPULSE_PROFILE_PASSENGER) {
        (void)apply_active_rules_profile(SAFEPULSE_PROFILE_PASSENGER);
    }
    actuator_slew_init(&g_slew);
    memset(&g_stats, 0, sizeof(g_stats));
    if (restored_violation != 0) {
        atomic_store_explicit((_Atomic uint32_t*)&g_stats.last_violation_code, restored_violation, memory_order_relaxed);
        g_stats.fsm_state = (uint32_t)g_fsm.current_state;
        g_last_slog_state = g_fsm.current_state;
        g_last_slog_viol = restored_violation;
    }

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
    iofunc_attr_init(&ioattr, S_IFCHR | 0660, 0, 0);
    IOFUNC_NOTIFY_INIT(g_notify);

    /* Bind handlers */
    connect_funcs.open = safepulse_io_open;
    io_funcs.read      = safepulse_io_read;
    io_funcs.write     = safepulse_io_write;
    io_funcs.devctl    = safepulse_io_devctl;
    io_funcs.notify    = safepulse_io_notify;
    io_funcs.close_ocb = safepulse_io_close_ocb;

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

    /* Configure 10 ms autonomous dispatch timeout so starvation watchdog ticks even during total bus silence */
    struct timespec wd_timeout = { .tv_sec = 0, .tv_nsec = 10000000L };
    if (dispatch_timeout(dpp, &wd_timeout) != 0) {
        fprintf(stderr, "[SafePulse] Warning: dispatch_timeout setup failed (%s)\n", strerror(errno));
    }

    printf("[SafePulse] Device registered successfully at %s\n", RESMGR_DEVICE_NAME);
    printf("[SafePulse] Ready to receive telemetry stream...\n");
    sp_slog(SLOG2_INFO, "Device %s ready sched_policy=%u prio=%u",
            RESMGR_DEVICE_NAME, (unsigned)g_active_sched_policy, (unsigned)g_active_sched_prio);

    /* 3. Microkernel Event Message Loop */
    while (1) {
        errno = 0;
        dispatch_context_t *ready_ctp = dispatch_block(ctp);
        if (!ready_ctp) {
            if (errno == ETIMEDOUT || errno == EINTR || errno == EOK) {
                struct timespec wd_ts;
                clock_gettime(CLOCK_MONOTONIC, &wd_ts);
                uint64_t wd_now_ns = ((uint64_t)wd_ts.tv_sec * 1000000000ULL) + (uint64_t)wd_ts.tv_nsec;
                check_and_persist_starvation_at(wd_now_ns);
                continue;
            }
            fprintf(stderr, "dispatch_block error: %s\n", strerror(errno));
            break;
        }
        ctp = ready_ctp;
        dispatch_handler(ctp);
    }

    return EXIT_SUCCESS;
}
