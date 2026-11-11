#include <stdio.h>
#include "safepulse_api.h"

int main(void) {
    printf("==================================================\n");
    printf(" SafePulse Custom Rules (Commercial Truck Profile)\n");
    printf("==================================================\n");

    safepulse_ctx_t *ctx = safepulse_init();
    if (!ctx) return 1;

    /* Load custom vehicle rules from YAML */
    if (safepulse_load_rules_file(ctx, "truck_rules.yaml") != 0 &&
        safepulse_load_rules_file(ctx, "examples/custom_rules/truck_rules.yaml") != 0 &&
        safepulse_load_rules_file(ctx, "/proc/boot/truck_rules.yaml") != 0) {
        fprintf(stderr, "Error: Failed to load truck_rules.yaml\n");
        safepulse_destroy(ctx);
        return 1;
    }

    /* Test Air Brake packet: 180 bar fails default 150 bar limit, passes custom 250 bar truck limit */
    safepulse_packet_t pkt = {0};
    pkt.seq_num = 1;
    pkt.timestamp_ns = 1000000ULL;
    pkt.sensor_type = SAFEPULSE_SENSOR_BRAKE;
    pkt.val_primary = 180.0; /* 180 bar (Valid for Heavy Truck air brakes) */
    safepulse_seal_packet(&pkt, 0);

    safepulse_verdict_t verdict;
    safepulse_inspect(ctx, &pkt, &verdict);

    printf("Air Brake Inspection: %s (Score: %.1f, State: %d)\n",
           verdict.passed ? "PASS" : "FAIL", verdict.safety_score, verdict.fsm_state);

    safepulse_destroy(ctx);
    return verdict.passed ? 0 : 1;
}
