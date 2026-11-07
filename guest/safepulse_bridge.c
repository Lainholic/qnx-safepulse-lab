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
#include <arpa/inet.h>
#include <signal.h>

#include "safepulse.h"
#include "safepulse_devctl.h"

#define BRIDGE_TCP_PORT 9876

static uint32_t g_active_fault = 0;
static uint64_t g_fault_end_ms = 0;

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    int dev_fd;
    int server_sock, client_sock = -1;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;
    uint32_t seq = 0;
    double sim_time = 0.0;

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

    /* 3. Main Telemetry & Control Loop (runs at 50 Hz = 20ms period) */
    while (1) {
        /* Check for new client connection if not connected */
        if (client_sock < 0) {
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
            if (client_sock >= 0) {
                printf("[Bridge] Host Dashboard connected from %s:%d\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                fcntl(client_sock, F_SETFL, O_NONBLOCK);
                /* Instantly synchronize to real-time live edge via devctl */
                devctl(dev_fd, DCMD_SAFEPULSE_SYNC_EDGE, NULL, 0, NULL);
                printf("[Bridge] Synchronized to live telemetry stream edge via DCMD_SAFEPULSE_SYNC_EDGE.\n");
            }
        }

        /* Check if Host sent a 16-byte Chaos Injection Command */
        if (client_sock >= 0) {
            chaos_command_t cmd;
            ssize_t n = recv(client_sock, &cmd, sizeof(cmd), MSG_DONTWAIT);
            if (n == (ssize_t)sizeof(cmd)) {
                if (cmd.magic == CHAOS_MAGIC) {
                    printf("[Bridge] Received Chaos Injection: Type=%u, Duration=%u ms\n",
                           cmd.fault_type, cmd.duration_ms);
                    g_active_fault = cmd.fault_type;
                    g_fault_end_ms = (get_time_ns() / 1000000ULL) + cmd.duration_ms;
                    /* Forward chaos command to /dev/safepulse */
                    if (dev_fd >= 0) {
                        write(dev_fd, &cmd, sizeof(cmd));
                    }
                }
            } else if (n == 0) {
                printf("[Bridge] Host disconnected.\n");
                close(client_sock);
                client_sock = -1;
            }
        }

        /* Only inject chaos fault packet when an explicit fault is triggered */
        uint64_t now_ms = get_time_ns() / 1000000ULL;
        int fault_active = (g_active_fault != 0 && now_ms < g_fault_end_ms);

        if (fault_active) {
            seq++;
            /* 1. Steering Sensor Fault */
            if (g_active_fault == 1 || g_active_fault == 2 || g_active_fault == 4) {
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
                    usleep(120000);
                    pkt.val_primary = 35.0;
                    pkt.val_secondary = 50.0;
                }
                pkt.crc32 = safepulse_crc32((const uint8_t*)&pkt, offsetof(safepulse_packet_t, crc32));
                if (g_active_fault == 4) {
                    pkt.crc32 ^= 0xDEADBEEF;
                }
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

        /* Read out verified packets from /dev/safepulse ringbuffer and stream to Host */
        if (client_sock >= 0) {
            safepulse_packet_t out_pkt;
            while (read(dev_fd, &out_pkt, sizeof(out_pkt)) == (ssize_t)sizeof(out_pkt)) {
                ssize_t sent = send(client_sock, &out_pkt, sizeof(out_pkt), 0);
                if (sent <= 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        printf("[Bridge] Client send error or disconnect (errno=%d). Closing socket.\n", errno);
                        close(client_sock);
                        client_sock = -1;
                        break;
                    }
                }
            }
        }

        /* 20ms period (50 Hz) */
        usleep(20000);
    }

    close(dev_fd);
    close(server_sock);
    return 0;
}
