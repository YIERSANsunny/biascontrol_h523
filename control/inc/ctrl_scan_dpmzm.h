#ifndef CTRL_SCAN_DPMZM_H
#define CTRL_SCAN_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "ctrl_measure_dpmzm.h"

typedef enum {
    DPMZM_SCAN_STAGE_MATP = 0,
    DPMZM_SCAN_STAGE_QTP,
    DPMZM_SCAN_STAGE_MITP,
    DPMZM_SCAN_STAGE_COUNT
} dpmzm_scan_stage_t;

typedef enum {
    DPMZM_SCAN_TARGET_I = 0,
    DPMZM_SCAN_TARGET_Q,
    DPMZM_SCAN_TARGET_P,
    DPMZM_SCAN_TARGET_COUNT
} dpmzm_scan_target_t;

typedef enum {
    DPMZM_SCAN_PILOT_EXTERNAL = 0,
    DPMZM_SCAN_PILOT_ONBOARD,
    DPMZM_SCAN_PILOT_COUNT
} dpmzm_scan_pilot_mode_t;

typedef enum {
    DPMZM_SCAN_DUMP_METRICS = 0,
    DPMZM_SCAN_DUMP_RAW,
    DPMZM_SCAN_DUMP_BOTH,
    DPMZM_SCAN_DUMP_COUNT
} dpmzm_scan_dump_mode_t;

typedef struct {
    dpmzm_scan_stage_t stage;
    dpmzm_scan_target_t target;

    float start_v;
    float stop_v;
    float step_v;
    uint32_t blocks;
    uint32_t settle_ms;

    float bias_i_v;
    float bias_q_v;
    float bias_p_v;

    uint8_t bias_i_dac_channel;
    uint8_t bias_q_dac_channel;
    uint8_t bias_p_dac_channel;

    float pilot_i_freq_hz;
    float pilot_q_freq_hz;
    float pilot_i_amp_v;
    float pilot_q_amp_v;
    dpmzm_scan_pilot_mode_t pilot_mode;
    dpmzm_scan_dump_mode_t dump_mode;
} dpmzm_scan_request_t;

typedef struct {
    bool valid;
    const char *primary_metric_name;
    float best_sweep_value;
    float best_metric_value;
    float secondary_best_sweep_value;
} dpmzm_scan_summary_t;

const char *dpmzm_scan_stage_name(dpmzm_scan_stage_t stage);
const char *dpmzm_scan_target_name(dpmzm_scan_target_t target);
const char *dpmzm_scan_pilot_mode_name(dpmzm_scan_pilot_mode_t mode);

bool dpmzm_scan_run(const dpmzm_scan_request_t *req,
                    dpmzm_scan_summary_t *summary_out);

#endif /* CTRL_SCAN_DPMZM_H */
