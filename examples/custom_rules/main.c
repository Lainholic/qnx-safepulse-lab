#include <stdio.h>
#include "safepulse_api.h"

int main(void) {
    printf("==================================================\n");
    printf(" SafePulse Custom Rules (Commercial Truck Profile)\n");
    printf("==================================================\n");

    safepulse_ctx_t *ctx = safepulse_init();
    if (!ctx) return 1;

    /* Load custom vehicle rules from YAML */
    if (safepulse_load_rules_file(ctx, "truck_rules.yaml") != 0) {
        printf("Note: Using default or inline rules\n");
    }

    /* Test Air Brake packet */
    safepulse_packet_t pkt = {0};
    pkt.magic = SAFEPULSE_MAGIC;
    pkt.sensor_type = SAFEPULSE_SENSOR_BRAKE;
    pkt.val_primary = 180.0; /* 180 bar (Valid for Heavy Truck air brakes) */

    safepulse_verdict_t verdict;
    safepulse_inspect(ctx, &pkt, &verdict);

    printf("Air Brake Inspection: %s (Score: %.1f, State: %d)\n",
           verdict.passed ? "PASS" : "FAIL", verdict.safety_score, verdict.fsm_state);

    safepulse_destroy(ctx);
    return 0;
}
