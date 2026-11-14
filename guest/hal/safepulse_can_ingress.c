/**
 * @file safepulse_can_ingress.c
 * @brief CAN FD to SafePulse Gateway Ingress Ingestion Engine
 * Connects CAN FD physical frames to QNX SafePulse inspection pipeline.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "safepulse_can_ingress.h"

int safepulse_can_ingress_init(uint8_t channel) {
    hal_can_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nominal_bitrate = 500000;
    cfg.data_bitrate = 2000000;
    cfg.fd_enabled = true;
    cfg.sample_point_nom_pct = 80;
    cfg.sample_point_data_pct = 70;
    cfg.auto_bus_off_recovery = false; /* Manual ISO 11898-1 host authorization */
    return hal_can_init(channel, &cfg);
}

int safepulse_can_ingress_push_frame(uint8_t channel, const hal_can_frame_t *frame) {
    return hal_can_transmit(channel, frame);
}

int safepulse_can_ingress_send_packet(uint8_t channel, uint32_t can_id, const safepulse_packet_t *pkt) {
    if (!pkt) return -EINVAL;
    hal_can_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = can_id;
    frame.channel = channel;
    frame.is_can_fd = true;
    frame.bitrate_switch = true;
    frame.payload_len = sizeof(safepulse_packet_t);
    memcpy(frame.payload, pkt, sizeof(safepulse_packet_t));
    return hal_can_transmit(channel, &frame);
}

int safepulse_can_ingress_poll_packet(uint8_t channel, safepulse_packet_t *out_pkt) {
    if (!out_pkt) return -EINVAL;
    hal_can_frame_t frame;
    int rc = hal_can_receive(channel, &frame, 0);
    if (rc != 0) return rc;

    if (frame.payload_len != sizeof(safepulse_packet_t)) {
        return -EBADMSG;
    }
    memcpy(out_pkt, frame.payload, sizeof(safepulse_packet_t));
    return 0;
}

int safepulse_can_ingress_get_stats(uint8_t channel, hal_controller_stats_t *out_stats) {
    return hal_can_get_stats(channel, out_stats);
}
