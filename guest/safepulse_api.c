/**
 * SafePulse Embedded Safety SDK Implementation
 * Encapsulates Linter Engine, AUTOSAR E2E Profile 1, and Safe State FSM Watchdog
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "safepulse_api.h"
#include "safepulse.h"
#include "linter.h"
#include "safe_state_fsm.h"
#include "autosar_e2e.h"

struct safepulse_ctx {
    linter_state_t   linter;
    safe_state_fsm_t fsm;
};

static uint64_t sdk_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

safepulse_ctx_t *safepulse_init(void) {
    safepulse_ctx_t *ctx = (safepulse_ctx_t *)calloc(1, sizeof(safepulse_ctx_t));
    if (!ctx) return NULL;

    linter_init(&ctx->linter);
    safe_state_fsm_init(&ctx->fsm, sdk_get_time_ns());
    return ctx;
}

static bool extract_json_number_in_range(const char *start, const char *end, const char *key, double *out_val) {
    char quoted_key[64];
    int klen = snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
    if (klen <= 0 || (size_t)klen >= sizeof(quoted_key)) return false;

    const char *p = start;
    while (p < end) {
        const char *hit = strstr(p, quoted_key);
        if (!hit || hit >= end) return false;

        const char *q = hit + klen;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q < end) {
                char *num_end = NULL;
                double val = strtod(q, &num_end);
                if (num_end != q && num_end <= end) {
                    *out_val = val;
                    return true;
                }
            }
        }
        p = hit + klen;
    }
    return false;
}

static int parse_yaml_rules(safepulse_ctx_t *ctx, const char *content) {
    int applied = 0;
    int cur_type = -1;
    double cur_period_ms = -1.0;
    double cur_tol_ms = -1.0;

    const char *line = content;
    while (*line != '\0') {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[256];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1U;
        memcpy(buf, line, len);
        buf[len] = '\0';

        char *s = buf;
        while (*s != '\0' && isspace((unsigned char)*s)) s++;
        if (*s != '\0' && *s != '#') {
            if (strncmp(s, "- type:", 7) == 0) {
                cur_type = (int)strtol(s + 7, NULL, 0);
                cur_period_ms = -1.0;
                cur_tol_ms = -1.0;
            } else {
                char *colon = strchr(s, ':');
                if (colon) {
                    *colon = '\0';
                    const char *k = s;
                    const char *v = colon + 1;
                    while (*v != '\0' && isspace((unsigned char)*v)) v++;

                    if (strcmp(k, "starvation_limit_ms") == 0) {
                        double ms = strtod(v, NULL);
                        if (ms > 0.0) {
                            ctx->fsm.starvation_limit_ns = (uint64_t)(ms * 1000000.0);
                            applied++;
                        }
                    } else if (cur_type >= 0 && cur_type < (int)MAX_TRACKED_SENSORS) {
                        if (strcmp(k, "min_val") == 0) {
                            ctx->linter.rules[cur_type].min_bound = strtod(v, NULL);
                            applied++;
                        } else if (strcmp(k, "max_val") == 0) {
                            ctx->linter.rules[cur_type].max_bound = strtod(v, NULL);
                            applied++;
                        } else if (strcmp(k, "max_jerk") == 0) {
                            ctx->linter.rules[cur_type].max_jerk = strtod(v, NULL);
                            applied++;
                        } else if (strcmp(k, "deadline_ms") == 0) {
                            double dms = strtod(v, NULL);
                            if (dms > 0.0) {
                                ctx->linter.rules[cur_type].deadline_ns = (uint64_t)(dms * 1000000.0);
                                applied++;
                            }
                        } else if (strcmp(k, "period_ms") == 0) {
                            cur_period_ms = strtod(v, NULL);
                            if (cur_period_ms > 0.0 && cur_tol_ms >= 0.0) {
                                ctx->linter.rules[cur_type].deadline_ns = (uint64_t)((cur_period_ms + cur_tol_ms) * 1000000.0);
                                applied++;
                            }
                        } else if (strcmp(k, "deadline_tolerance_ms") == 0) {
                            cur_tol_ms = strtod(v, NULL);
                            if (cur_period_ms > 0.0 && cur_tol_ms >= 0.0) {
                                ctx->linter.rules[cur_type].deadline_ns = (uint64_t)((cur_period_ms + cur_tol_ms) * 1000000.0);
                                applied++;
                            }
                        }
                    }
                }
            }
        }

        if (!eol) break;
        line = eol + 1;
    }

    return (applied > 0) ? 0 : -2;
}

int safepulse_load_rules_json(safepulse_ctx_t *ctx, const char *config_content) {
    if (!ctx || !config_content) return -1;

    /* Delegate YAML configurations when no JSON root brace is present */
    if (!strchr(config_content, '{')) {
        return parse_yaml_rules(ctx, config_content);
    }

    const char *doc_end = config_content + strlen(config_content);
    int applied = 0;

    /* 1. Top-level watchdog starvation_limit_ms */
    double starv_ms = 0.0;
    if (extract_json_number_in_range(config_content, doc_end, "starvation_limit_ms", &starv_ms) && starv_ms > 0.0) {
        ctx->fsm.starvation_limit_ns = (uint64_t)(starv_ms * 1000000.0);
        applied++;
    }

    /* 2. Sensors array objects */
    const char *sensors_pos = strstr(config_content, "\"sensors\"");
    if (sensors_pos) {
        const char *arr_start = strchr(sensors_pos, '[');
        if (arr_start) {
            const char *p = arr_start + 1;
            while (p < doc_end) {
                while (p < doc_end && *p != '{' && *p != ']') p++;
                if (p >= doc_end || *p == ']') break;

                const char *obj_start = p;
                int depth = 0;
                while (p < doc_end) {
                    if (*p == '{') depth++;
                    else if (*p == '}') {
                        depth--;
                        if (depth == 0) {
                            p++;
                            break;
                        }
                    }
                    p++;
                }
                const char *obj_end = p;

                double type_d = -1.0;
                if (extract_json_number_in_range(obj_start, obj_end, "type", &type_d)) {
                    int stype = (int)type_d;
                    if (stype >= 0 && stype < (int)MAX_TRACKED_SENSORS) {
                        double val = 0.0;
                        if (extract_json_number_in_range(obj_start, obj_end, "min_val", &val)) {
                            ctx->linter.rules[stype].min_bound = val;
                            applied++;
                        }
                        if (extract_json_number_in_range(obj_start, obj_end, "max_val", &val)) {
                            ctx->linter.rules[stype].max_bound = val;
                            applied++;
                        }
                        if (extract_json_number_in_range(obj_start, obj_end, "max_jerk", &val)) {
                            ctx->linter.rules[stype].max_jerk = val;
                            applied++;
                        }
                        double deadline_ms = 0.0;
                        double period_ms = 0.0;
                        double tol_ms = 0.0;
                        if (extract_json_number_in_range(obj_start, obj_end, "deadline_ms", &deadline_ms) && deadline_ms > 0.0) {
                            ctx->linter.rules[stype].deadline_ns = (uint64_t)(deadline_ms * 1000000.0);
                            applied++;
                        } else if (extract_json_number_in_range(obj_start, obj_end, "period_ms", &period_ms) &&
                                   extract_json_number_in_range(obj_start, obj_end, "deadline_tolerance_ms", &tol_ms) &&
                                   period_ms > 0.0 && tol_ms >= 0.0) {
                            ctx->linter.rules[stype].deadline_ns = (uint64_t)((period_ms + tol_ms) * 1000000.0);
                            applied++;
                        }
                    }
                }
            }
        }
    }

    return (applied > 0) ? 0 : -2;
}

int safepulse_load_rules_file(safepulse_ctx_t *ctx, const char *file_path) {
    if (!ctx || !file_path) return -1;

    FILE *fp = fopen(file_path, "r");
    if (!fp) return -2;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 65536) {
        fclose(fp);
        return -3;
    }

    char *buf = (char *)malloc((size_t)sz + 1U);
    if (!buf) {
        fclose(fp);
        return -4;
    }

    size_t r = fread(buf, 1, (size_t)sz, fp);
    buf[r] = '\0';
    fclose(fp);

    int res = safepulse_load_rules_json(ctx, buf);
    free(buf);
    return res;
}

int safepulse_seal_packet(safepulse_packet_t *pkt, uint8_t alive_counter) {
    if (!pkt) return -1;

    pkt->magic = SAFEPULSE_MAGIC;
    pkt->safety_score = 100.0f;
    pkt->violation_code = VIOLATION_NONE;
    autosar_e2e_stamp_packet(pkt, alive_counter);
    pkt->crc32 = safepulse_crc32((const uint8_t *)pkt, offsetof(safepulse_packet_t, crc32));
    return 0;
}

static const char *get_violation_str(uint32_t code) {
    switch (code) {
        case VIOLATION_NONE: return "NONE";
        case VIOLATION_OUT_OF_BOUNDS: return "PHYSICAL_OUT_OF_BOUNDS (0x1001)";
        case VIOLATION_JERK_EXCEEDED: return "JERK_EXCEEDED (0x1002)";
        case VIOLATION_DEADLINE_MISS: return "DEADLINE_MISS (0x1003)";
        case VIOLATION_CRC_CORRUPTED: return "CRC_CORRUPTED (0x1004)";
        case VIOLATION_PLAUSIBILITY: return "PLAUSIBILITY_CONTRADICTION (0x1005)";
        case VIOLATION_E2E_REPEATED: return "E2E_ALIVE_REPEATED (0x1006)";
        case VIOLATION_E2E_SEQ_LOSS: return "E2E_ALIVE_SEQ_LOSS (0x1007)";
        default: return "UNKNOWN_VIOLATION";
    }
}

int safepulse_inspect(safepulse_ctx_t *ctx, safepulse_packet_t *pkt, safepulse_verdict_t *verdict) {
    if (!ctx || !pkt || !verdict) return -1;

    /* 1. Linter inspection (modifies pkt->safety_score, pkt->violation_code, pkt->status_flags in-place) */
    linter_inspect_packet(&ctx->linter, pkt);

    /* 2. Step Safe State FSM */
    asil_safe_state_t st = safe_state_fsm_step(&ctx->fsm, pkt->violation_code, pkt->safety_score, pkt->timestamp_ns);

    /* 3. Output verdict */
    verdict->passed = (pkt->violation_code == VIOLATION_NONE);
    verdict->violation_code = pkt->violation_code;
    verdict->safety_score = pkt->safety_score;
    verdict->fsm_state = (safepulse_fsm_state_t)st;
    verdict->violation_name = get_violation_str(pkt->violation_code);

    return 0;
}

safepulse_fsm_state_t safepulse_check_starvation(safepulse_ctx_t *ctx, uint64_t current_time_ns) {
    if (!ctx) return SAFEPULSE_FSM_EMERGENCY_STOP;
    safe_state_fsm_check_starvation(&ctx->fsm, current_time_ns);
    return (safepulse_fsm_state_t)ctx->fsm.current_state;
}

safepulse_fsm_state_t safepulse_get_fsm_state(safepulse_ctx_t *ctx) {
    if (!ctx) return SAFEPULSE_FSM_EMERGENCY_STOP;
    return (safepulse_fsm_state_t)ctx->fsm.current_state;
}

void safepulse_reset_fsm(safepulse_ctx_t *ctx) {
    if (!ctx) return;
    uint64_t saved_starv_ns = ctx->fsm.starvation_limit_ns;
    safe_state_fsm_init(&ctx->fsm, sdk_get_time_ns());
    if (saved_starv_ns > 0ULL) {
        ctx->fsm.starvation_limit_ns = saved_starv_ns;
    }
}

void safepulse_destroy(safepulse_ctx_t *ctx) {
    if (ctx) free(ctx);
}
