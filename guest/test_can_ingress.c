/**
 * @file test_can_ingress.c
 * @brief Automated verification of CAN FD Ingress & Bus-Off Recovery Pipeline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "safepulse_can_ingress.h"

int main(void) {
    printf("======================================================================\n");
    printf("SafePulse QNX 8.0 CAN FD Ingress & Bus-Off Pipeline Verification\n");
    printf("======================================================================\n");

    uint8_t ch = 0;
    int rc = safepulse_can_ingress_init(ch);
    printf("[Test 1] CAN FD Channel %u Initialization: %s\n", ch, (rc == 0) ? "PASS" : "FAIL");
    assert(rc == 0);

    /* Test 2: Nominal 64-byte SafePulse Packet Ingress */
    safepulse_packet_t tx_pkt;
    memset(&tx_pkt, 0, sizeof(tx_pkt));
    tx_pkt.magic = SAFEPULSE_MAGIC;
    tx_pkt.seq_num = 1001;
    tx_pkt.sensor_type = SENSOR_TYPE_STEERING;
    tx_pkt.val_primary = 15.5;
    tx_pkt.val_secondary = 25.0;
    tx_pkt.safety_score = 100.0f;
    tx_pkt.crc32 = safepulse_crc32((const uint8_t*)&tx_pkt, offsetof(safepulse_packet_t, crc32));

    rc = safepulse_can_ingress_send_packet(ch, 0x120, &tx_pkt);
    printf("[Test 2] CAN FD 64-byte Packet Transmit: %s (rc=%d)\n", (rc == 0) ? "PASS" : "FAIL", rc);
    assert(rc == 0);

    safepulse_packet_t rx_pkt;
    rc = safepulse_can_ingress_poll_packet(ch, &rx_pkt);
    printf("[Test 3] CAN FD Packet Poll & Ingress Deserialization: %s (seq=%u, magic=0x%08X)\n",
           (rc == 0 && rx_pkt.seq_num == 1001 && rx_pkt.magic == SAFEPULSE_MAGIC) ? "PASS" : "FAIL",
           rx_pkt.seq_num, rx_pkt.magic);
    assert(rc == 0);
    assert(rx_pkt.seq_num == 1001);
    assert(rx_pkt.magic == SAFEPULSE_MAGIC);

    /* Test 4: Physical Bus-Off Trip & Error Containment */
    hal_can_inject_fault(ch, HAL_FAULT_BUS_OFF_TRIP);
    hal_controller_stats_t stats;
    safepulse_can_ingress_get_stats(ch, &stats);
    printf("[Test 4] Physical Bus-Off Trip (TEC=%u, State=%u): %s\n",
           stats.tec, stats.bus_state, (stats.bus_state == HAL_BUS_STATE_BUS_OFF) ? "PASS" : "FAIL");
    assert(stats.bus_state == HAL_BUS_STATE_BUS_OFF);

    /* Attempt transmit during bus-off: must fail with -ENETDOWN */
    rc = safepulse_can_ingress_send_packet(ch, 0x121, &tx_pkt);
    printf("[Test 5] Transmit Rejection during Bus-Off: %s (rc=%d)\n",
           (rc == -ENETDOWN) ? "PASS" : "FAIL", rc);
    assert(rc == -ENETDOWN);

    /* Test 6: ISO 11898-1 Recessive Recovery */
    rc = hal_can_trigger_bus_off_recovery(ch);
    safepulse_can_ingress_get_stats(ch, &stats);
    printf("[Test 6] ISO 11898-1 128x11 Recessive Recovery: %s (State=%u, Recoveries=%u)\n",
           (rc == 0 && stats.bus_state == HAL_BUS_STATE_ACTIVE) ? "PASS" : "FAIL",
           stats.bus_state, stats.bus_off_recoveries);
    assert(rc == 0);
    assert(stats.bus_state == HAL_BUS_STATE_ACTIVE);

    printf("======================================================================\n");
    printf("ALL 6 CAN FD INGRESS & BUS-OFF TESTS PASSED (100.0%%)\n");
    printf("======================================================================\n");
    return 0;
}
