#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <spawn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/syspage.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <dirent.h>
#include <devctl.h>
#include <sys/procfs.h>
#ifndef DCMD_PROC_TIDSTATUS
#define DCMD_PROC_TIDSTATUS DCMD_PROC_TIDSTATUS64
#endif
#include <secpol/secpol.h>

#include "safepulse.h"
#include "safepulse_devctl.h"
#include "autosar_e2e.h"
#include "safepulse_identity.h"

#define BRIDGE_TCP_PORT 9876

static uint32_t g_active_fault = 0;
static uint64_t g_fault_end_ms = 0;
static uint8_t  g_recovery_epoch = 0;
static int      g_replay_mode = 0;
static slog2_buffer_t g_slog_buf = NULL;

static void init_bridge_slog2(void) {
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffer_set_name = "safepulse_bridge";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "bridge";
    cfg.buffer_config[0].num_pages = 2;
    (void)slog2_register(&cfg, &g_slog_buf, SLOG2_QUIET);
}

static void __attribute__((format(printf, 2, 3)))
bridge_slog(uint8_t severity, const char *fmt, ...) {
    if (!g_slog_buf) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vslog2f(g_slog_buf, 0, severity, fmt, ap);
    va_end(ap);
}

static void signal_ecus(int sig, const char *sig_label, const char *only_ecu_name) {
    DIR *d = opendir("/proc");
    if (!d) {
        fprintf(stderr, "[Bridge] Failed to open /proc for %s: %s\n", sig_label, strerror(errno));
        return;
    }

    int matched = 0;
    int signaled = 0;
    pid_t self_pid = getpid();
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (end == de->d_name || *end != '\0' || pid <= 1 || (pid_t)pid == self_pid) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/ctl", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        struct { procfs_debuginfo info; char buf[256]; } map;
        memset(&map, 0, sizeof(map));
        int rc = devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL);
        close(fd);
        if (rc != EOK) {
            continue;
        }
        const char *base = strrchr(map.info.path, '/');
        base = base ? base + 1 : map.info.path;
        int target_match = only_ecu_name
            ? (strcmp(base, only_ecu_name) == 0)
            : (strcmp(base, "ecu_steering") == 0 ||
               strcmp(base, "ecu_braking") == 0 ||
               strcmp(base, "ecu_powertrain") == 0);
        if (target_match) {
            matched++;
            if (kill((pid_t)pid, sig) == 0) {
                signaled++;
            } else {
                fprintf(stderr, "[Bridge] kill(%ld, %s) failed: %s\n", pid, sig_label, strerror(errno));
            }
        }
    }
    closedir(d);
    printf("[Bridge] Sent %s to %d/%d virtual ECUs (%s) via /proc\n",
           sig_label, signaled, matched, only_ecu_name ? only_ecu_name : "all");
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void clear_resmgr_latch_quiet(int dev_fd) {
    if (dev_fd >= 0) {
        chaos_command_t clr;
        memset(&clr, 0, sizeof(clr));
        clr.magic = CHAOS_MAGIC;
        clr.fault_type = 0;
        clr.duration_ms = 0;
        clr.checksum = CHAOS_MAGIC;
        (void)write(dev_fd, &clr, sizeof(clr));
    }
}

static void set_replay_mode(int enable, int dev_fd) {
    if (enable && !g_replay_mode) {
        printf("[Bridge] Entering Real-Vehicle Replay Mode: Stopping 3 virtual ECUs...\n");
        signal_ecus(SIGSTOP, "SIGSTOP", NULL);
        usleep(15000);
        if (dev_fd >= 0) {
            clear_resmgr_latch_quiet(dev_fd);
            devctl(dev_fd, DCMD_SAFEPULSE_SYNC_EDGE, NULL, 0, NULL);
            devctl(dev_fd, DCMD_SAFEPULSE_RESET_STATS, NULL, 0, NULL);
        }
        g_replay_mode = 1;
    } else if (!enable && g_replay_mode) {
        printf("[Bridge] Exiting Real-Vehicle Replay Mode: Resuming 3 virtual ECUs...\n");
        g_replay_mode = 0;
        clear_resmgr_latch_quiet(dev_fd);
        signal_ecus(SIGCONT, "SIGCONT", NULL);
        clear_resmgr_latch_quiet(dev_fd);
    }
}

static int recv_exact_bytes(int sock, uint8_t *buf, size_t len, int timeout_ms) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(sock, buf + got, len - got, MSG_DONTWAIT);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return 0; /* Disconnected */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                struct pollfd pfd = { .fd = sock, .events = POLLIN };
                int pr = poll(&pfd, 1, timeout_ms);
                if (pr <= 0) {
                    return -1;
                }
            } else {
                return -1;
            }
        }
    }
    return (int)got;
}

static int send_exact_bytes(int sock, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(sock, buf + off, len - off, 0);
        if (s > 0) {
            off += (size_t)s;
        } else if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pfd = { .fd = sock, .events = POLLOUT };
            if (poll(&pfd, 1, 20) <= 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

static void run_rogue_demo(int dev_fd, int client_sock) {
    safepulse_rogue_result_t result = {0};
    result.magic = SAFEPULSE_ROGUE_RESULT_MAGIC;
    result.open_write_error = result.write_error = result.open_reset_error = -1;
    result.reset_error = result.signal_error = -1;
    uint32_t pid = 0;
    int err = dev_fd < 0 ? EBADF : devctl(dev_fd, DCMD_SAFEPULSE_GET_PID, &pid, sizeof(pid), NULL);
    int fds[2] = {-1, -1};
    if (!err && pipe(fds) != 0) err = errno;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    int attr_initialized = 0, actions_initialized = 0;
    pid_t child = -1;
    if (!err) { err = posix_spawnattr_init(&attr); attr_initialized = err == 0; }
    if (!err) { err = posix_spawn_file_actions_init(&actions); actions_initialized = err == 0; }
    if (!err) err = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    if (!err) err = posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (!err) err = posix_spawn_file_actions_addclose(&actions, fds[1]);
    if (!err) err = posix_spawnattr_setcred(&attr, SAFEPULSE_UID_ROGUE, SAFEPULSE_GID_DEVICE);
    if (!err && secpol_posix_spawnattr_settypeid(NULL, &attr, "safepulse_rogue_t", SECPOL_TYPE_NAME) != 0)
        err = errno ? errno : EINVAL;
    uint32_t flags = 0;
    if (!err) err = posix_spawnattr_getxflags(&attr, &flags);
    if (!err) err = posix_spawnattr_setxflags(&attr, flags | POSIX_SPAWN_SETCRED);
    char pid_text[24];
    snprintf(pid_text, sizeof(pid_text), "%u", pid);
    char *args[] = {"/proc/boot/rogue_writer", "--wire", pid_text, NULL};
    if (!err) err = posix_spawn(&child, args[0], &actions, &attr, args, NULL);
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    if (attr_initialized) posix_spawnattr_destroy(&attr);
    if (fds[1] >= 0) close(fds[1]);
    if (!err) {
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        if (poll(&pfd, 1, 2000) <= 0) err = ETIMEDOUT;
        else {
            size_t received = 0;
            while (received < sizeof(result)) {
                ssize_t n = read(fds[0], (uint8_t *)&result + received, sizeof(result) - received);
                if (n <= 0) { err = n < 0 ? errno : EIO; break; }
                received += (size_t)n;
            }
            if (!err && result.magic != SAFEPULSE_ROGUE_RESULT_MAGIC) err = EPROTO;
        }
    }
    if (fds[0] >= 0) close(fds[0]);
    if (child > 0) (void)waitpid(child, NULL, WNOHANG);
    if (err) {
        memset(&result, 0, sizeof(result));
        result.magic = SAFEPULSE_ROGUE_RESULT_MAGIC;
        result.open_write_error = result.write_error = result.open_reset_error = -1;
        result.reset_error = result.signal_error = -1;
        result.launch_error = err;
    }
    (void)send_exact_bytes(client_sock, (const uint8_t *)&result, sizeof(result));
}

/* =========================================================================
 * QM CPU-overload run (Freedom From Interference, ISO 26262-6 Annex D)
 * Low-priority QM threads saturate every CPU and thrash memory for a few
 * seconds while the safety path keeps running at its own priority.
 * ========================================================================= */
#define LOAD_ATTACK_MS      5000U
#define LOAD_ATTACK_PRIO    8      /* Below the bridge (10), ECUs and /dev/safepulse (50) */
#define LOAD_MAX_THREADS    4

static volatile int      g_load_running = 0;
static int               g_load_active = 0;
static uint64_t          g_load_start_ms = 0;
static pthread_t         g_load_threads[LOAD_MAX_THREADS];
static int               g_load_nthreads = 0;
static volatile uint64_t g_load_counts[LOAD_MAX_THREADS];
static uint32_t          g_load_start_inspected = 0;
static uint32_t          g_load_start_violations = 0;
static int               g_load_error = 0;

static void *load_math_worker(void *arg) {
    int idx = (int)(intptr_t)arg;
    volatile double x = 1.0001;
    while (g_load_running) {
        for (int k = 0; k < 1000; k++) x = sin(x) * cos(x) + sqrt(x * 1.5 + 0.1);
        g_load_counts[idx]++;
    }
    return NULL;
}

static void start_load_attack(int dev_fd) {
    if (g_load_active) return;
    safepulse_stats_t st;
    memset(&st, 0, sizeof(st));
    if (dev_fd >= 0) {
        (void)devctl(dev_fd, DCMD_SAFEPULSE_RESET_LATENCY, NULL, 0, NULL);
        (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st, sizeof(st), NULL);
    }
    g_load_start_inspected = st.total_inspected;
    g_load_start_violations = st.total_violations;

    int ncpu = (int)_syspage_ptr->num_cpu;
    if (ncpu < 1) ncpu = 1;
    if (ncpu > LOAD_MAX_THREADS) ncpu = LOAD_MAX_THREADS;

    pthread_attr_t attr;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = LOAD_ATTACK_PRIO;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);

    g_load_running = 1;
    g_load_nthreads = 0;
    g_load_error = 0;
    /* CPU-only load: a memory-thrash thread stalls the whole QEMU guest on the Windows host, which is an emulator artifact, not QNX scheduling. */
    for (int i = 0; i < ncpu; i++) {
        g_load_counts[i] = 0;
        int err = pthread_create(&g_load_threads[i], &attr, load_math_worker, (void *)(intptr_t)i);
        if (err != 0) { g_load_error = err; break; }
        g_load_nthreads++;
    }
    pthread_attr_destroy(&attr);
    g_load_start_ms = get_time_ns() / 1000000ULL;
    g_load_active = 1;
    printf("[Bridge] QM load: %d threads at priority %d for %u ms (err=%d)\n",
           g_load_nthreads, LOAD_ATTACK_PRIO, LOAD_ATTACK_MS, g_load_error);
}

static void finish_load_attack(int dev_fd, int client_sock) {
    g_load_running = 0;
    uint64_t total = 0;
    for (int i = 0; i < g_load_nthreads; i++) {
        (void)pthread_join(g_load_threads[i], NULL);
        total += g_load_counts[i];
    }
    uint64_t now_ms = get_time_ns() / 1000000ULL;

    safepulse_stats_t st;
    memset(&st, 0, sizeof(st));
    if (dev_fd >= 0) (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st, sizeof(st), NULL);

    safepulse_load_result_t r;
    memset(&r, 0, sizeof(r));
    r.magic = SAFEPULSE_LOAD_RESULT_MAGIC;
    r.duration_ms = (uint32_t)(now_ms - g_load_start_ms);
    r.threads = (uint32_t)g_load_nthreads;
    r.load_priority = LOAD_ATTACK_PRIO;
    r.load_iterations = total;
    r.packets_inspected = st.total_inspected - g_load_start_inspected;
    r.violations = st.total_violations - g_load_start_violations;
    r.max_inspect_ns = st.max_inspect_ns;
    r.max_gap_ns = st.max_gap_ns;
    r.fsm_state = st.fsm_state;
    r.safety_priority = st.sched_priority;
    r.error = g_load_error;
    printf("[Bridge] QM load done: %llu iterations, %u packets, max inspect %u ns, max gap %u ns, fsm=%u\n",
           (unsigned long long)total, r.packets_inspected, r.max_inspect_ns, r.max_gap_ns, r.fsm_state);
    if (client_sock >= 0) (void)send_exact_bytes(client_sock, (const uint8_t *)&r, sizeof(r));
    g_load_active = 0;
}

static void drain_ringbuffer_to_client(int *dev_fd_ptr, int *client_sock_ptr) {
    int dev_fd = *dev_fd_ptr;
    int client_sock = *client_sock_ptr;
    if (client_sock < 0 || dev_fd < 0) return;

    safepulse_stats_t live_stats;
    memset(&live_stats, 0, sizeof(live_stats));
    int have_stats = (devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &live_stats, sizeof(live_stats), NULL) == 0);
    if (!have_stats) {
        close(dev_fd);
        dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
        *dev_fd_ptr = dev_fd;
        if (dev_fd >= 0) {
            have_stats = (devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &live_stats, sizeof(live_stats), NULL) == 0);
            if (!g_replay_mode) {
                devctl(dev_fd, DCMD_SAFEPULSE_SYNC_EDGE, NULL, 0, NULL);
            }
            printf("[Bridge] Reconnected to respawned /dev/safepulse\n");
        } else {
            return;
        }
    }

    safepulse_packet_t out_pkt;
    while (1) {
        ssize_t rret = read(dev_fd, &out_pkt, sizeof(out_pkt));
        if (rret == (ssize_t)sizeof(out_pkt)) {
            if (have_stats) {
                out_pkt.reserved[8]  = (uint8_t)live_stats.fsm_state;
                out_pkt.reserved[9]  = (uint8_t)live_stats.sched_policy;
                out_pkt.reserved[10] = (uint8_t)live_stats.sched_priority;
                out_pkt.reserved[11] = SAFEPULSE_STATE_MARKER;
            }
            {
                uint16_t ecu_starv_bits = 0U;
                if (have_stats && out_pkt.violation_code == VIOLATION_STARVATION &&
                    live_stats.starved_sensor >= 1U && live_stats.starved_sensor <= 3U) {
                    ecu_starv_bits = (uint16_t)((live_stats.starved_sensor & 0x03U) << 4);
                }
                out_pkt.status_flags = (out_pkt.status_flags & 0x00CFU) | ecu_starv_bits | ((uint16_t)g_recovery_epoch << 8);
            }
            out_pkt.crc32 = safepulse_crc32((const uint8_t*)&out_pkt, offsetof(safepulse_packet_t, crc32));
            if (send_exact_bytes(client_sock, (const uint8_t *)&out_pkt, sizeof(out_pkt)) != 0) {
                printf("[Bridge] Client send error or disconnect (errno=%d). Closing socket.\n", errno);
                close(client_sock);
                *client_sock_ptr = -1;
                set_replay_mode(0, dev_fd);
                break;
            }
        } else {
            if (rret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(dev_fd);
                *dev_fd_ptr = -1;
            }
            break;
        }
    }
}

/* Live process view: read SafePulse processes through /proc/<pid>/ctl (only UIDs the security policy lets the bridge query). */
#define PROC_VIEW_MAX     12
#define PROC_VIEW_PERIOD_MS 1000U
static uint32_t g_proc_cycle = 0;
static uint64_t g_proc_last_ms = 0;

static int read_proc_entry(int pid, safepulse_proc_entry_t *e) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ctl", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct { procfs_debuginfo info; char buf[256]; } map;
    memset(&map, 0, sizeof(map));
    if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL) != EOK) { close(fd); return -1; }
    const char *base = strrchr(map.info.path, '/');
    base = base ? base + 1 : map.info.path;
    if (strncmp(base, "ecu_", 4) != 0 && strncmp(base, "safepulse_", 10) != 0) { close(fd); return -1; }
    procfs_info info;
    memset(&info, 0, sizeof(info));
    if (devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), NULL) != EOK) { close(fd); return -1; }

    memset(e, 0, sizeof(*e));
    e->magic = SAFEPULSE_PROC_MAGIC;
    e->pid = pid;
    e->cpu_ns = info.utime + info.stime;
    strncpy(e->name, base, sizeof(e->name) - 1);
    e->prio_min = 255;
    int tid = 1, n = 0;
    while (n < 64) {
        procfs_status ts;
        memset(&ts, 0, sizeof(ts));
        ts.tid = tid;
        if (devctl(fd, DCMD_PROC_TIDSTATUS, &ts, sizeof(ts), NULL) != EOK) break;
        if ((int)ts.tid < tid) break;
        if (n == 0) e->state_main = ts.state;
        if (ts.priority >= e->prio_max) { e->prio_max = ts.priority; e->policy_max = ts.policy; }
        if (ts.priority <= e->prio_min) { e->prio_min = ts.priority; e->policy_min = ts.policy; }
        n++;
        tid = (int)ts.tid + 1;
    }
    close(fd);
    if (n == 0) e->prio_min = 0;
    e->threads = (uint16_t)n;
    return 0;
}

/* Rolling timing window: read the maxima, then reset them (left alone during the QM load run, which owns them). */
static uint64_t g_timing_last_ms = 0;
static uint32_t g_timing_last_inspected = 0;
static uint8_t  g_timing_last_epoch = 0;

static void send_timing_window(int dev_fd, int *client_sock_ptr) {
    if (*client_sock_ptr < 0 || dev_fd < 0) return;
    uint64_t now_ms = get_time_ns() / 1000000ULL;
    if (g_timing_last_ms == 0) {
        g_timing_last_ms = now_ms;
        (void)devctl(dev_fd, DCMD_SAFEPULSE_RESET_LATENCY, NULL, 0, NULL);
        return;
    }
    if (now_ms - g_timing_last_ms < 1000U) return;
    safepulse_stats_t st;
    memset(&st, 0, sizeof(st));
    if (devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st, sizeof(st), NULL) != EOK) return;
    safepulse_timing_t tm;
    memset(&tm, 0, sizeof(tm));
    tm.magic = SAFEPULSE_TIMING_MAGIC;
    tm.window_ms = (uint32_t)(now_ms - g_timing_last_ms);
    tm.max_inspect_ns = st.max_inspect_ns;
    tm.max_gap_ns = st.max_gap_ns;
    /* A respawned /dev/safepulse starts its counter from 0: count from the respawn instead of wrapping. */
    if (g_recovery_epoch != g_timing_last_epoch || st.total_inspected < g_timing_last_inspected) {
        tm.inspected = st.total_inspected;
        tm.restarted = 1U;
    } else {
        tm.inspected = st.total_inspected - g_timing_last_inspected;
    }
    tm.fsm_state = st.fsm_state;
    tm.load_active = g_load_active ? 1U : 0U;
    tm.reserved[0] = (uint8_t)st.active_profile;
    tm.reserved[1] = (uint8_t)st.starved_sensor;
    {
        uint16_t wd_ms = (uint16_t)(st.starvation_limit_ms > 0U ? st.starvation_limit_ms : 50U);
        memcpy(&tm.reserved[2], &wd_ms, sizeof(wd_ms));
    }
    g_timing_last_inspected = st.total_inspected;
    g_timing_last_epoch = g_recovery_epoch;
    g_timing_last_ms = now_ms;
    if (!g_load_active) (void)devctl(dev_fd, DCMD_SAFEPULSE_RESET_LATENCY, NULL, 0, NULL);
    (void)send_exact_bytes(*client_sock_ptr, (const uint8_t *)&tm, sizeof(tm));
}

static void send_process_view(int *client_sock_ptr) {
    if (*client_sock_ptr < 0) return;
    uint64_t now_ms = get_time_ns() / 1000000ULL;
    if (now_ms - g_proc_last_ms < PROC_VIEW_PERIOD_MS) return;
    g_proc_last_ms = now_ms;

    safepulse_proc_entry_t rows[PROC_VIEW_MAX];
    int count = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < PROC_VIEW_MAX) {
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (end == de->d_name || *end != '\0' || pid <= 1) continue;
        if (read_proc_entry((int)pid, &rows[count]) == 0) count++;
    }
    closedir(d);
    g_proc_cycle++;
    for (int i = 0; i < count; i++) {
        rows[i].cycle = g_proc_cycle;
        rows[i].index = (uint16_t)i;
        rows[i].count = (uint16_t)count;
        if (send_exact_bytes(*client_sock_ptr, (const uint8_t *)&rows[i], sizeof(rows[i])) != 0) return;
    }
}

int main(void) {
    int dev_fd;
    int server_sock, client_sock = -1;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;
    uint32_t seq = 0;

    printf("==================================================\n");
    printf(" Starting SafePulse QNX <-> Host TCP Bridge \n");
    printf(" Port: %d (Virtual VirtIO Port)\n", BRIDGE_TCP_PORT);
    printf("==================================================\n");

    /* Ignore SIGPIPE so disconnects do not kill the daemon */
    signal(SIGPIPE, SIG_IGN);
    init_bridge_slog2();
    bridge_slog(SLOG2_INFO, "SafePulse Bridge initializing TCP:%d with QNX _IO_NOTIFY multiplexing", BRIDGE_TCP_PORT);

    /* 1. Open /dev/safepulse */
    dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
    if (dev_fd < 0) {
        fprintf(stderr, "[Bridge] Error opening /dev/safepulse: %s\n", strerror(errno));
        return 1;
    }
    printf("[Bridge] Successfully connected to /dev/safepulse\n");

    /* 2. Setup TCP Server Socket */
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(BRIDGE_TCP_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_sock, 2) < 0) {
        perror("listen");
        return 1;
    }

    /* Set server socket to non-blocking */
    fcntl(server_sock, F_SETFL, O_NONBLOCK);
    printf("[Bridge] Listening on 0.0.0.0:%d. Ready for Host Dashboard connection.\n", BRIDGE_TCP_PORT);

    /* 3. Main Telemetry & Control Loop */
    while (1) {
        /* Check for new client connection if not connected */
        if (client_sock < 0) {
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
            if (client_sock >= 0) {
                printf("[Bridge] Host Dashboard connected from %s:%d\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                bridge_slog(SLOG2_INFO, "Host connected from %s:%d",
                            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                fcntl(client_sock, F_SETFL, O_NONBLOCK);
                /* Instantly synchronize to real-time live edge via devctl */
                if (dev_fd >= 0) {
                    devctl(dev_fd, DCMD_SAFEPULSE_SYNC_EDGE, NULL, 0, NULL);
                }
                printf("[Bridge] Synchronized to live telemetry stream edge via DCMD_SAFEPULSE_SYNC_EDGE.\n");
            }
        }

        /* Process all incoming frames on client_sock by inspecting leading 4-byte magic */
        while (client_sock >= 0) {
            uint8_t magic_buf[4];
            ssize_t n = recv(client_sock, magic_buf, sizeof(magic_buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    break;
                }
                printf("[Bridge] Host recv error (errno=%d). Closing socket.\n", errno);
                close(client_sock);
                client_sock = -1;
                set_replay_mode(0, dev_fd);
                break;
            }
            if (n == 0) {
                printf("[Bridge] Host disconnected.\n");
                close(client_sock);
                client_sock = -1;
                set_replay_mode(0, dev_fd);
                break;
            }

            chaos_command_t cmd;
            int have_chaos_cmd = 0;

            if (n == 1 && magic_buf[0] <= 20U) {
                /* Legacy 1-byte fault code shorthand */
                memset(&cmd, 0, sizeof(cmd));
                cmd.magic = CHAOS_MAGIC;
                cmd.fault_type = (uint32_t)magic_buf[0];
                cmd.duration_ms = 100;
                cmd.checksum = CHAOS_MAGIC ^ cmd.fault_type ^ cmd.duration_ms;
                have_chaos_cmd = 1;
            } else {
                if (n < 4) {
                    int rem = recv_exact_bytes(client_sock, magic_buf + n, 4U - (size_t)n, 50);
                    if (rem <= 0) {
                        if (rem == 0) {
                            close(client_sock);
                            client_sock = -1;
                            set_replay_mode(0, dev_fd);
                        }
                        break;
                    }
                }

                uint32_t magic = 0;
                memcpy(&magic, magic_buf, sizeof(uint32_t));

                if (magic == CHAOS_MAGIC) {
                    memset(&cmd, 0, sizeof(cmd));
                    memcpy(&cmd, magic_buf, 4U);
                    int rem = recv_exact_bytes(client_sock, ((uint8_t *)&cmd) + 4U, sizeof(chaos_command_t) - 4U, 50);
                    if (rem <= 0) {
                        if (rem == 0) {
                            close(client_sock);
                            client_sock = -1;
                            set_replay_mode(0, dev_fd);
                        }
                        break;
                    }
                    have_chaos_cmd = 1;
                } else if (magic == SAFEPULSE_MAGIC) {
                    safepulse_packet_t in_pkt;
                    memset(&in_pkt, 0, sizeof(in_pkt));
                    memcpy(&in_pkt, magic_buf, 4U);
                    int rem = recv_exact_bytes(client_sock, ((uint8_t *)&in_pkt) + 4U, sizeof(safepulse_packet_t) - 4U, 50);
                    if (rem <= 0) {
                        if (rem == 0) {
                            close(client_sock);
                            client_sock = -1;
                            set_replay_mode(0, dev_fd);
                        }
                        break;
                    }

                    /* Pause virtual ECUs automatically when real-vehicle replay packets arrive */
                    if (!g_replay_mode) {
                        set_replay_mode(1, dev_fd);
                    }

                    if (dev_fd < 0) {
                        dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
                    }
                    if (dev_fd >= 0) {
                        ssize_t wret = write(dev_fd, &in_pkt, sizeof(in_pkt));
                        if (wret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            close(dev_fd);
                            dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
                            if (dev_fd >= 0) {
                                (void)write(dev_fd, &in_pkt, sizeof(in_pkt));
                            }
                        }
                    }
                    drain_ringbuffer_to_client(&dev_fd, &client_sock);
                } else {
                    /* Unknown magic header: ignore malformed 4 bytes */
                    continue;
                }
            }

            if (have_chaos_cmd && cmd.magic == CHAOS_MAGIC) {
                if (cmd.fault_type == 14) {
                    run_rogue_demo(dev_fd, client_sock);
                    continue;
                } else if (cmd.fault_type == 15) {
                    start_load_attack(dev_fd);
                    continue;
                } else if (cmd.fault_type == 12) {
                    set_replay_mode(1, dev_fd);
                    continue;
                } else if (cmd.fault_type == 13) {
                    set_replay_mode(0, dev_fd);
                    continue;
                } else if (cmd.fault_type == 17) {
                    uint32_t prof = (cmd.duration_ms == 1U) ? SAFEPULSE_PROFILE_TRUCK : SAFEPULSE_PROFILE_PASSENGER;
                    if (dev_fd < 0) dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
                    if (dev_fd >= 0) {
                        int prc = devctl(dev_fd, DCMD_SAFEPULSE_LOAD_PROFILE, &prof, sizeof(prof), NULL);
                        printf("[Bridge] Dynamic profile switch -> %u (rc=%d, errno=%d)\n",
                               (unsigned)prof, prc, prc == 0 ? 0 : errno);
                        bridge_slog((prc == 0) ? SLOG2_NOTICE : SLOG2_WARNING,
                                    "Dynamic vehicle profile -> %s (rc=%d)",
                                    (prof == SAFEPULSE_PROFILE_TRUCK) ? "TRUCK" : "PASSENGER", prc);
                    }
                    g_active_fault = 0;
                    continue;
                } else if (cmd.fault_type == 16) {
                    if (!g_replay_mode) {
                        safepulse_stats_t st_check;
                        memset(&st_check, 0, sizeof(st_check));
                        if (dev_fd >= 0) {
                            (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                        }
                        uint32_t min_silence = (st_check.starvation_limit_ms > 50U) ? 260U : 150U;
                        uint32_t freeze_ms = (cmd.duration_ms >= min_silence && cmd.duration_ms <= 1000U)
                            ? cmd.duration_ms : min_silence;
                        printf("[Bridge] Fault 16: Freezing ONLY ecu_steering for %u ms (Brake/Throttle active)...\n",
                               (unsigned)freeze_ms);
                        bridge_slog(SLOG2_WARNING, "Fault 16: Freezing ecu_steering for %u ms", (unsigned)freeze_ms);
                        signal_ecus(SIGSTOP, "SIGSTOP", "ecu_steering");
                        for (uint32_t elapsed = 0; elapsed < freeze_ms; elapsed += 20U) {
                            usleep(20000);
                            drain_ringbuffer_to_client(&dev_fd, &client_sock);
                        }
                        if (dev_fd >= 0) {
                            (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                        }
                        signal_ecus(SIGCONT, "SIGCONT", "ecu_steering");
                        drain_ringbuffer_to_client(&dev_fd, &client_sock);
                    }
                    g_active_fault = 0;
                    continue;
                }

                printf("[Bridge] Received Chaos Injection: Type=%u, Duration=%u ms\n",
                       cmd.fault_type, cmd.duration_ms);
                if (!g_replay_mode) {
                    g_active_fault = cmd.fault_type;
                    g_fault_end_ms = (get_time_ns() / 1000000ULL) + cmd.duration_ms;
                }

                /* Ensure /dev/safepulse fd is alive before forwarding command */
                safepulse_stats_t tmp_stats;
                if (dev_fd < 0 || devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &tmp_stats, sizeof(tmp_stats), NULL) != 0) {
                    if (dev_fd >= 0) close(dev_fd);
                    dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
                }
                if (dev_fd >= 0) {
                    if (cmd.fault_type == 3 || cmd.fault_type == 6 || cmd.fault_type == 7 || cmd.fault_type == 8) {
                        /* Flush any pending inspected packets before inducing fatal resmgr crash */
                        drain_ringbuffer_to_client(&dev_fd, &client_sock);
                    }
                    (void)write(dev_fd, &cmd, sizeof(cmd));
                    if (cmd.fault_type == 3 || cmd.fault_type == 6 || cmd.fault_type == 7 || cmd.fault_type == 8) {
                        /* Fatal crash injected into resmgr: close dead fd and poll for watcher respawn */
                        g_active_fault = 0;
                        close(dev_fd);
                        dev_fd = -1;
                        for (int retry = 0; retry < 4000; retry++) {
                            usleep(500);
                            dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
                            if (dev_fd >= 0) {
                                if (devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &tmp_stats, sizeof(tmp_stats), NULL) == 0) {
                                    if (!g_replay_mode) {
                                        devctl(dev_fd, DCMD_SAFEPULSE_SYNC_EDGE, NULL, 0, NULL);
                                    }
                                    g_recovery_epoch++;
                                    printf("[Bridge] Reconnected to respawned /dev/safepulse (epoch=%u)\n",
                                           (unsigned)g_recovery_epoch);
                                    bridge_slog(SLOG2_NOTICE, "Reconnected to respawned /dev/safepulse epoch=%u",
                                                (unsigned)g_recovery_epoch);
                                    break;
                                }
                                close(dev_fd);
                                dev_fd = -1;
                            }
                        }
                        /* In nominal virtual-ECU mode only, wait up to 25ms for first post-respawn ECU packet */
                        if (!g_replay_mode && dev_fd >= 0) {
                            for (int w = 0; w < 25; w++) {
                                if (devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &tmp_stats, sizeof(tmp_stats), NULL) == 0 &&
                                    tmp_stats.ringbuffer_count > 0) {
                                    break;
                                }
                                usleep(1000);
                            }
                        }
                    }
                }
                if (cmd.fault_type == 11) {
                    if (!g_replay_mode) {
                        /* Fault 11: Simulate Total ECU Bus Silence (> starvation_limit_ms) */
                        safepulse_stats_t st_check;
                        memset(&st_check, 0, sizeof(st_check));
                        if (dev_fd >= 0) {
                            (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                        }
                        uint32_t silence_us = (st_check.starvation_limit_ms > 50U) ? 130000U : 80000U;
                        printf("[Bridge] Fault 11: Freezing all virtual ECUs for %u ms to test Starvation Watchdog...\n",
                               (unsigned)(silence_us / 1000U));
                        signal_ecus(SIGSTOP, "SIGSTOP", NULL);
                        usleep(silence_us);
                        if (dev_fd >= 0) {
                            (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                        }
                        signal_ecus(SIGCONT, "SIGCONT", NULL);
                    } else if (dev_fd >= 0) {
                        safepulse_stats_t st_check;
                        (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                    }
                    g_active_fault = 0;
                }
            }
        }

        /* Only inject synthetic chaos fault packet when in virtual-ECU mode */
        uint64_t now_ms = get_time_ns() / 1000000ULL;
        int fault_active = (!g_replay_mode && g_active_fault != 0 && now_ms < g_fault_end_ms);

        if (fault_active) {
            seq++;
            /* 1. Steering Sensor Fault (1=Bounds, 2=Deadline, 4=AUTOSAR E2E Bitflip, 9=E2E Counter Skip, 10=E2E Counter Repeat) */
            if (g_active_fault == 1 || g_active_fault == 2 || g_active_fault == 4 || g_active_fault == 9 || g_active_fault == 10) {
                safepulse_packet_t pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.magic = SAFEPULSE_MAGIC;
                pkt.seq_num = seq;
                pkt.timestamp_ns = get_time_ns();
                pkt.sensor_type = SENSOR_TYPE_STEERING;

                if (g_active_fault == 1) {
                    pkt.val_primary = 850.0;
                    pkt.val_secondary = 120.0;
                } else if (g_active_fault == 2) {
                    pkt.timestamp_ns += 120000000ULL;
                    pkt.val_primary = 35.0;
                    pkt.val_secondary = 50.0;
                    g_active_fault = 0;
                } else if (g_active_fault == 4) {
                    pkt.val_primary = 25.0;
                    pkt.val_secondary = 10.0;
                    autosar_e2e_stamp_packet(&pkt, (uint8_t)(seq & 0x0F));
                    pkt.reserved[4] ^= 0x01U;
                } else if (g_active_fault == 9) {
                    pkt.sensor_type = SENSOR_TYPE_IMU_YAW;
                    pkt.val_primary = 10.0;
                    pkt.val_secondary = 1.0;
                    autosar_e2e_stamp_packet(&pkt, 1U);
                    pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
                    if (dev_fd >= 0) write(dev_fd, &pkt, sizeof(pkt));
                    pkt.seq_num = ++seq;
                    pkt.timestamp_ns = get_time_ns();
                    autosar_e2e_stamp_packet(&pkt, 6U);
                } else if (g_active_fault == 10) {
                    pkt.sensor_type = SENSOR_TYPE_IMU_YAW;
                    pkt.val_primary = 10.0;
                    pkt.val_secondary = 1.0;
                    autosar_e2e_stamp_packet(&pkt, 7U);
                    pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
                    if (dev_fd >= 0) write(dev_fd, &pkt, sizeof(pkt));
                    pkt.seq_num = ++seq;
                    pkt.timestamp_ns = get_time_ns();
                    autosar_e2e_stamp_packet(&pkt, 7U);
                }
                pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
                if (dev_fd >= 0) write(dev_fd, &pkt, sizeof(pkt));
            }

            /* 2. Throttle Sensor Fault */
            if (g_active_fault == 5) {
                safepulse_packet_t pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.magic = SAFEPULSE_MAGIC;
                pkt.seq_num = seq;
                pkt.timestamp_ns = get_time_ns();
                pkt.sensor_type = SENSOR_TYPE_THROTTLE;
                pkt.val_primary = 95.0;
                pkt.val_secondary = 0.0;
                pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
                if (dev_fd >= 0) write(dev_fd, &pkt, sizeof(pkt));
            }
        }

        /* Re-open /dev/safepulse if the resource manager was respawned after a crash */
        if (dev_fd < 0) {
            dev_fd = open("/dev/safepulse", O_RDWR | O_NONBLOCK);
            if (dev_fd >= 0) {
                printf("[Bridge] Reconnected to respawned /dev/safepulse\n");
            }
        }

        /* Read out verified packets from /dev/safepulse ringbuffer and stream to Host */
        drain_ringbuffer_to_client(&dev_fd, &client_sock);

        if (g_load_active && (get_time_ns() / 1000000ULL) - g_load_start_ms >= LOAD_ATTACK_MS) {
            finish_load_attack(dev_fd, client_sock);
        }

        send_process_view(&client_sock);
        send_timing_window(dev_fd, &client_sock);

        if (client_sock >= 0) {
            struct pollfd pfds[2];
            nfds_t nfds = 0;
            pfds[nfds].fd = client_sock;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
            if (dev_fd >= 0) {
                /* Arms QNX _IO_NOTIFY kernel sigevent on /dev/safepulse instead of blind polling */
                pfds[nfds].fd = dev_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }
            int wait_ms = fault_active ? 5 : (g_load_active ? 20 : 100);
            (void)poll(pfds, nfds, wait_ms);
        } else {
            usleep(20000);
        }
    }

    close(dev_fd);
    close(server_sock);
    return 0;
}
