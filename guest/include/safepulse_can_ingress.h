#ifndef SAFEPULSE_CAN_INGRESS_H
#define SAFEPULSE_CAN_INGRESS_H

#include <stdint.h>
#include <stdbool.h>
#include "safepulse.h"
#include "safepulse_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

int safepulse_can_ingress_init(uint8_t channel);
int safepulse_can_ingress_push_frame(uint8_t channel, const hal_can_frame_t *frame);
int safepulse_can_ingress_send_packet(uint8_t channel, uint32_t can_id, const safepulse_packet_t *pkt);
int safepulse_can_ingress_poll_packet(uint8_t channel, safepulse_packet_t *out_pkt);
int safepulse_can_ingress_get_stats(uint8_t channel, hal_controller_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* SAFEPULSE_CAN_INGRESS_H */
