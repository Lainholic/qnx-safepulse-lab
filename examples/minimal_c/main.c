#include <stdio.h>
#include "safepulse_api.h"

int main(void) {
    safepulse_ctx_t *ctx = safepulse_init();
    if (!ctx) return 1;

    safepulse_packet_t pkt = {0};
    pkt.seq_num = 1;
    pkt.timestamp_ns = 1000000ULL;
    pkt.sensor_type = SAFEPULSE_SENSOR_STEERING;
    pkt.val_primary = 25.0; /* 25 degrees (Normal) */
    safepulse_seal_packet(&pkt, 0);

    safepulse_verdict_t verdict;
    safepulse_inspect(ctx, &pkt, &verdict);

    printf("Safety Verdict: %s (Score: %.1f)\n",
           verdict.passed ? "PASS" : "FAIL", verdict.safety_score);

    safepulse_destroy(ctx);
    return verdict.passed ? 0 : 1;
}
