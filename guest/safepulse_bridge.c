#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include "safepulse.h"
#include "safepulse_devctl.h"
#include "autosar_e2e.h"

#define BRIDGE_TCP_PORT 9876

static uint32_t g_active_fault = 0;
static uint64_t g_fault_end_ms = 0;
static uint8_t  g_recovery_epoch = 0;
static int      g_replay_mode = 0;

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
        system("slay -s SIGSTOP ecu_steering ecu_braking ecu_powertrain >/dev/null 2>&1");
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
        system("slay -s SIGCONT ecu_steering ecu_braking ecu_powertrain >/dev/null 2>&1");
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
            out_pkt.status_flags = (out_pkt.status_flags & 0x00FFU) | ((uint16_t)g_recovery_epoch << 8);
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
                if (cmd.fault_type == 12) {
                    set_replay_mode(1, dev_fd);
                    continue;
                } else if (cmd.fault_type == 13) {
                    set_replay_mode(0, dev_fd);
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
                        /* Fault 11: Simulate Total ECU Bus Silence (> 50ms STARVATION_LIMIT_NS) */
                        printf("[Bridge] Fault 11: Freezing all virtual ECUs for %u ms to test Starvation Watchdog...\n", cmd.duration_ms);
                        system("slay -s SIGSTOP ecu_steering ecu_braking ecu_powertrain >/dev/null 2>&1");
                        usleep(80000); /* 80ms total bus silence (>= 50ms STARVATION_LIMIT_NS) */
                        if (dev_fd >= 0) {
                            safepulse_stats_t st_check;
                            (void)devctl(dev_fd, DCMD_SAFEPULSE_GET_STATS, &st_check, sizeof(st_check), NULL);
                        }
                        system("slay -s SIGCONT ecu_steering ecu_braking ecu_powertrain >/dev/null 2>&1");
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

        if (client_sock >= 0) {
            struct pollfd pfd = { .fd = client_sock, .events = POLLIN };
            (void)poll(&pfd, 1, 5);
        } else {
            usleep(20000);
        }
    }

    close(dev_fd);
    close(server_sock);
    return 0;
}
