#include "app_main_dpmzm.h"
#include "ctrl_scan_dpmzm.h"
#include "drv_dac8568.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_dpmzm_context_t s_dpmzm_ctx;

static const char *pilot_source_name(dpmzm_pilot_source_t source)
{
    switch (source) {
    case DPMZM_PILOT_SOURCE_EXTERNAL:
        return "external";
    case DPMZM_PILOT_SOURCE_ONBOARD:
        return "onboard";
    default:
        return "unknown";
    }
}

static const char *dump_mode_name(dpmzm_dump_mode_t mode)
{
    switch (mode) {
    case DPMZM_DUMP_METRICS:
        return "metrics";
    case DPMZM_DUMP_RAW:
        return "raw";
    case DPMZM_DUMP_BOTH:
        return "both";
    default:
        return "unknown";
    }
}

static dpmzm_scan_pilot_mode_t to_scan_pilot_mode(dpmzm_pilot_source_t source)
{
    switch (source) {
    case DPMZM_PILOT_SOURCE_EXTERNAL:
        return DPMZM_SCAN_PILOT_EXTERNAL;
    case DPMZM_PILOT_SOURCE_ONBOARD:
        return DPMZM_SCAN_PILOT_ONBOARD;
    default:
        return DPMZM_SCAN_PILOT_ONBOARD;
    }
}

static dpmzm_scan_dump_mode_t to_scan_dump_mode(dpmzm_dump_mode_t mode)
{
    switch (mode) {
    case DPMZM_DUMP_METRICS:
        return DPMZM_SCAN_DUMP_METRICS;
    case DPMZM_DUMP_RAW:
        return DPMZM_SCAN_DUMP_RAW;
    case DPMZM_DUMP_BOTH:
        return DPMZM_SCAN_DUMP_BOTH;
    default:
        return DPMZM_SCAN_DUMP_METRICS;
    }
}

static int apply_biases(void)
{
    int ret_i = dac8568_set_voltage(s_dpmzm_ctx.config->bias_i_dac_channel,
                                    s_dpmzm_ctx.bias_i_v);
    int ret_q = dac8568_set_voltage(s_dpmzm_ctx.config->bias_q_dac_channel,
                                    s_dpmzm_ctx.bias_q_v);
    int ret_p = dac8568_set_voltage(s_dpmzm_ctx.config->bias_p_dac_channel,
                                    s_dpmzm_ctx.bias_p_v);

    if (ret_i != 0) {
        return ret_i;
    }
    if (ret_q != 0) {
        return ret_q;
    }
    return ret_p;
}

static bool parse_float_arg(const char *text, float *value_out)
{
    char *endptr = NULL;
    float value;

    if (text == NULL || value_out == NULL) {
        return false;
    }

    value = strtof(text, &endptr);
    if (endptr == text) {
        return false;
    }

    *value_out = value;
    return true;
}

static void print_status(void)
{
    printf("[dpmzm] status\r\n");
    printf("  initialized: %s\r\n", s_dpmzm_ctx.initialized ? "yes" : "no");
    printf("  bias channels: I=%u Q=%u P=%u\r\n",
           (unsigned)s_dpmzm_ctx.config->bias_i_dac_channel,
           (unsigned)s_dpmzm_ctx.config->bias_q_dac_channel,
           (unsigned)s_dpmzm_ctx.config->bias_p_dac_channel);
    printf("  bias voltage:  I=%+.3fV Q=%+.3fV P=%+.3fV\r\n",
           (double)s_dpmzm_ctx.bias_i_v,
           (double)s_dpmzm_ctx.bias_q_v,
           (double)s_dpmzm_ctx.bias_p_v);
    printf("  pilot source:  %s\r\n",
           pilot_source_name(s_dpmzm_ctx.config->pilot_source));
    printf("  pilot I:       %.1f Hz, %.1f mVpp\r\n",
           (double)s_dpmzm_ctx.config->pilot_i_freq_hz,
           (double)(s_dpmzm_ctx.config->pilot_i_amp_v * 2000.0f));
    printf("  pilot Q:       %.1f Hz, %.1f mVpp\r\n",
           (double)s_dpmzm_ctx.config->pilot_q_freq_hz,
           (double)(s_dpmzm_ctx.config->pilot_q_amp_v * 2000.0f));
    printf("  dump mode:     %s\r\n",
           dump_mode_name(s_dpmzm_ctx.config->dump_mode));
    printf("  scan blocks:   %lu\r\n",
           (unsigned long)s_dpmzm_ctx.config->scan_default_blocks);
}

static void handle_set_bias(const char *cmd)
{
    char path = '\0';
    float value = 0.0f;
    const char *value_str = NULL;
    int ret;

    if (strlen(cmd) < 12u) {
        printf("[dpmzm] usage: set bias i|q|p <voltage>\r\n");
        return;
    }

    path = cmd[9];
    value_str = cmd + 11;
    if (!parse_float_arg(value_str, &value)) {
        printf("[dpmzm] usage: set bias i|q|p <voltage>\r\n");
        return;
    }

    if (value < -10.0f) value = -10.0f;
    if (value >  10.0f) value =  10.0f;

    switch (path) {
    case 'i':
        s_dpmzm_ctx.bias_i_v = value;
        break;
    case 'q':
        s_dpmzm_ctx.bias_q_v = value;
        break;
    case 'p':
        s_dpmzm_ctx.bias_p_v = value;
        break;
    default:
        printf("[dpmzm] usage: set bias i|q|p <voltage>\r\n");
        return;
    }

    ret = apply_biases();
    printf("[dpmzm] bias %c -> %+.3f V (ret=%d)\r\n",
           path, (double)value, ret);
}

static void handle_set_pilot_src(const char *cmd)
{
    const char *arg = cmd + strlen("set pilot-src ");

    if (strcmp(arg, "onboard") == 0) {
        s_dpmzm_ctx.config->pilot_source = DPMZM_PILOT_SOURCE_ONBOARD;
    } else if (strcmp(arg, "external") == 0) {
        printf("[dpmzm] external pilot source is reserved but disabled in the current plan\r\n");
        printf("[dpmzm] use onboard DAC-generated pilots\r\n");
        return;
    } else {
        printf("[dpmzm] usage: set pilot-src onboard\r\n");
        return;
    }

    printf("[dpmzm] pilot source -> %s\r\n",
           pilot_source_name(s_dpmzm_ctx.config->pilot_source));
}

static void handle_set_pilot(const char *cmd)
{
    char path = '\0';
    float freq_hz = 0.0f;
    float mvpp = 0.0f;
    const char *args = cmd + strlen("set pilot ");
    const char *p = args;
    char *endptr = NULL;

    if (*p == '\0') {
        printf("[dpmzm] usage: set pilot i|q <freq_hz> <mVpp>\r\n");
        return;
    }

    path = *p++;
    if (*p != ' ') {
        printf("[dpmzm] usage: set pilot i|q <freq_hz> <mVpp>\r\n");
        return;
    }

    while (*p == ' ') {
        p++;
    }

    freq_hz = strtof(p, &endptr);
    if (endptr == p) {
        printf("[dpmzm] usage: set pilot i|q <freq_hz> <mVpp>\r\n");
        return;
    }

    p = endptr;
    while (*p == ' ') {
        p++;
    }

    mvpp = strtof(p, &endptr);
    if (endptr == p) {
        printf("[dpmzm] usage: set pilot i|q <freq_hz> <mVpp>\r\n");
        return;
    }

    if (freq_hz <= 0.0f || mvpp <= 0.0f) {
        printf("[dpmzm] pilot frequency and amplitude must be positive\r\n");
        return;
    }

    switch (path) {
    case 'i':
        s_dpmzm_ctx.config->pilot_i_freq_hz = freq_hz;
        s_dpmzm_ctx.config->pilot_i_amp_v = mvpp / 2000.0f;
        printf("[dpmzm] pilot I -> %.1f Hz, %.1f mVpp\r\n",
               (double)freq_hz, (double)mvpp);
        break;
    case 'q':
        s_dpmzm_ctx.config->pilot_q_freq_hz = freq_hz;
        s_dpmzm_ctx.config->pilot_q_amp_v = mvpp / 2000.0f;
        printf("[dpmzm] pilot Q -> %.1f Hz, %.1f mVpp\r\n",
               (double)freq_hz, (double)mvpp);
        break;
    default:
        printf("[dpmzm] usage: set pilot i|q <freq_hz> <mVpp>\r\n");
        break;
    }
}

static void handle_set_dump(const char *cmd)
{
    const char *arg = cmd + strlen("set dump ");

    if (strcmp(arg, "metrics") == 0) {
        s_dpmzm_ctx.config->dump_mode = DPMZM_DUMP_METRICS;
    } else if (strcmp(arg, "raw") == 0) {
        s_dpmzm_ctx.config->dump_mode = DPMZM_DUMP_RAW;
    } else if (strcmp(arg, "both") == 0) {
        s_dpmzm_ctx.config->dump_mode = DPMZM_DUMP_BOTH;
    } else {
        printf("[dpmzm] usage: set dump metrics|raw|both\r\n");
        return;
    }

    printf("[dpmzm] dump mode -> %s\r\n",
           dump_mode_name(s_dpmzm_ctx.config->dump_mode));
}

static void handle_scan_placeholder(const char *cmd)
{
    char cmd_copy[96];
    char *tokens[8] = {0};
    char *tok = NULL;
    int token_count = 0;
    dpmzm_scan_request_t req;
    dpmzm_scan_summary_t summary;
    float start_v = 0.0f;
    float stop_v = 0.0f;
    float step_v = 0.0f;
    unsigned long blocks = 0UL;
    bool ok;

    if (strlen(cmd) >= sizeof(cmd_copy)) {
        printf("[dpmzm] scan command too long\r\n");
        return;
    }

    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1U);
    cmd_copy[sizeof(cmd_copy) - 1U] = '\0';

    tok = strtok(cmd_copy, " ");
    while (tok != NULL && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[token_count++] = tok;
        tok = strtok(NULL, " ");
    }

    if (token_count < 6) {
        printf("[dpmzm] usage: scan matp|qtp|mitp i|q|p <start> <stop> <step> [blocks]\r\n");
        return;
    }

    memset(&req, 0, sizeof(req));

    if (strcmp(tokens[1], "matp") == 0) {
        req.stage = DPMZM_SCAN_STAGE_MATP;
    } else if (strcmp(tokens[1], "qtp") == 0) {
        req.stage = DPMZM_SCAN_STAGE_QTP;
    } else if (strcmp(tokens[1], "mitp") == 0) {
        req.stage = DPMZM_SCAN_STAGE_MITP;
    } else {
        printf("[dpmzm] unknown scan stage: %s\r\n", tokens[1]);
        return;
    }

    if (strcmp(tokens[2], "i") == 0) {
        req.target = DPMZM_SCAN_TARGET_I;
    } else if (strcmp(tokens[2], "q") == 0) {
        req.target = DPMZM_SCAN_TARGET_Q;
    } else if (strcmp(tokens[2], "p") == 0) {
        req.target = DPMZM_SCAN_TARGET_P;
    } else {
        printf("[dpmzm] unknown scan target: %s\r\n", tokens[2]);
        return;
    }

    if ((req.stage == DPMZM_SCAN_STAGE_QTP && req.target != DPMZM_SCAN_TARGET_P) ||
        (req.stage != DPMZM_SCAN_STAGE_QTP && req.target == DPMZM_SCAN_TARGET_P)) {
        printf("[dpmzm] stage/target mismatch\r\n");
        printf("         qtp only supports target p; matp/mitp only support i or q\r\n");
        return;
    }

    if (!parse_float_arg(tokens[3], &start_v) ||
        !parse_float_arg(tokens[4], &stop_v) ||
        !parse_float_arg(tokens[5], &step_v)) {
        printf("[dpmzm] usage: scan matp|qtp|mitp i|q|p <start> <stop> <step> [blocks]\r\n");
        return;
    }

    if (token_count >= 7) {
        blocks = strtoul(tokens[6], NULL, 10);
        if (blocks == 0UL) {
            printf("[dpmzm] blocks must be a positive integer\r\n");
            return;
        }
    } else {
        blocks = s_dpmzm_ctx.config->scan_default_blocks;
    }

    req.start_v = start_v;
    req.stop_v = stop_v;
    req.step_v = step_v;
    req.blocks = (uint32_t)blocks;
    req.settle_ms = 2U;
    req.bias_i_v = s_dpmzm_ctx.bias_i_v;
    req.bias_q_v = s_dpmzm_ctx.bias_q_v;
    req.bias_p_v = s_dpmzm_ctx.bias_p_v;
    req.bias_i_dac_channel = s_dpmzm_ctx.config->bias_i_dac_channel;
    req.bias_q_dac_channel = s_dpmzm_ctx.config->bias_q_dac_channel;
    req.bias_p_dac_channel = s_dpmzm_ctx.config->bias_p_dac_channel;
    req.pilot_i_freq_hz = s_dpmzm_ctx.config->pilot_i_freq_hz;
    req.pilot_q_freq_hz = s_dpmzm_ctx.config->pilot_q_freq_hz;
    req.pilot_i_amp_v = s_dpmzm_ctx.config->pilot_i_amp_v;
    req.pilot_q_amp_v = s_dpmzm_ctx.config->pilot_q_amp_v;
    req.pilot_mode = to_scan_pilot_mode(s_dpmzm_ctx.config->pilot_source);
    req.dump_mode = to_scan_dump_mode(s_dpmzm_ctx.config->dump_mode);

    printf("[dpmzm] scan start: stage=%s target=%s start=%+.3f stop=%+.3f step=%+.3f blocks=%lu\r\n",
           dpmzm_scan_stage_name(req.stage),
           dpmzm_scan_target_name(req.target),
           (double)req.start_v,
           (double)req.stop_v,
           (double)req.step_v,
           blocks);

    ok = dpmzm_scan_run(&req, &summary);
    (void)apply_biases();
    if (!ok) {
        printf("[dpmzm] scan failed\r\n");
        return;
    }

    printf("[dpmzm] scan done: best %s at %+.6f V (metric=%.9f)\r\n",
           summary.primary_metric_name,
           (double)summary.best_sweep_value,
           (double)summary.best_metric_value);
}

void app_dpmzm_init(void)
{
    app_config_dpmzm_defaults();
    s_dpmzm_ctx.config = app_config_dpmzm_get();
    s_dpmzm_ctx.bias_i_v = s_dpmzm_ctx.config->bias_i_initial_v;
    s_dpmzm_ctx.bias_q_v = s_dpmzm_ctx.config->bias_q_initial_v;
    s_dpmzm_ctx.bias_p_v = s_dpmzm_ctx.config->bias_p_initial_v;
    s_dpmzm_ctx.initialized = true;
}

void app_dpmzm_run(void)
{
    /* Placeholder: no background work in the first open-loop version. */
}

const app_dpmzm_context_t *app_dpmzm_get_context(void)
{
    return &s_dpmzm_ctx;
}

void app_dpmzm_handle_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }

    if (!s_dpmzm_ctx.initialized) {
        app_dpmzm_init();
    }

    if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strncmp(cmd, "set bias ", 9) == 0) {
        handle_set_bias(cmd);
    } else if (strncmp(cmd, "set pilot-src ", 14) == 0) {
        handle_set_pilot_src(cmd);
    } else if (strncmp(cmd, "set pilot ", 10) == 0) {
        handle_set_pilot(cmd);
    } else if (strncmp(cmd, "set dump ", 9) == 0) {
        handle_set_dump(cmd);
    } else if (strncmp(cmd, "scan matp ", 10) == 0 ||
               strncmp(cmd, "scan qtp ", 9) == 0 ||
               strncmp(cmd, "scan mitp ", 10) == 0) {
        handle_scan_placeholder(cmd);
    } else {
        printf("[dpmzm] unknown command: %s\r\n", cmd);
    }
}
