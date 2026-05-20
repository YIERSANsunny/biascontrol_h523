#ifndef CTRL_SCAN_DPMZM_H
#define CTRL_SCAN_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "ctrl_measure_dpmzm.h"

typedef int (*dpmzm_scan_bias_apply_fn_t)(float vi, float vq, float vp);

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
    bool continuous_onboard_pilot;
    dpmzm_scan_bias_apply_fn_t bias_apply_fn;
} dpmzm_scan_request_t;

typedef struct {
    bool valid;
    const char *primary_metric_name;
    float best_sweep_value;
    float best_metric_value;
    float secondary_best_sweep_value;
} dpmzm_scan_summary_t;

typedef struct {
    float sweep_v;
    float mag_fi;
    float mag_fq;
    float mag_fdiff;
    float mag_fsum;
    float dc_mean;
} dpmzm_scan_point_t;

typedef enum {
    DPMZM_SCAN_TRACE_IDLE = 0,
    DPMZM_SCAN_TRACE_POINT_START,
    DPMZM_SCAN_TRACE_APPLY_BIAS,
    DPMZM_SCAN_TRACE_SETTLE,
    DPMZM_SCAN_TRACE_DISCARD,
    DPMZM_SCAN_TRACE_MEASURE,
    DPMZM_SCAN_TRACE_PROCESS,
    DPMZM_SCAN_TRACE_FINALIZE,
    DPMZM_SCAN_TRACE_RESTORE,
    DPMZM_SCAN_TRACE_DONE,
    DPMZM_SCAN_TRACE_ERROR
} dpmzm_scan_trace_phase_t;

typedef enum {
    DPMZM_SCAN_TRACE_ERR_NONE = 0,
    DPMZM_SCAN_TRACE_ERR_BAD_ARG,
    DPMZM_SCAN_TRACE_ERR_BIAS_APPLY,
    DPMZM_SCAN_TRACE_ERR_ADC_DRDY_TIMEOUT,
    DPMZM_SCAN_TRACE_ERR_ADC_READ,
    DPMZM_SCAN_TRACE_ERR_DISCARD,
    DPMZM_SCAN_TRACE_ERR_PILOT_BIAS_APPLY,
    DPMZM_SCAN_TRACE_ERR_FINALIZE,
    DPMZM_SCAN_TRACE_ERR_POINT_OVERFLOW
} dpmzm_scan_trace_error_t;

typedef struct {
    volatile uint32_t generation;
    volatile bool running;
    volatile dpmzm_scan_trace_phase_t phase;
    volatile dpmzm_scan_trace_error_t last_error;
    volatile dpmzm_scan_stage_t stage;
    volatile dpmzm_scan_target_t target;
    volatile dpmzm_scan_pilot_mode_t pilot_mode;
    volatile dpmzm_scan_dump_mode_t dump_mode;
    volatile float sweep_v;
    volatile float base_i_v;
    volatile float base_q_v;
    volatile float base_p_v;
    volatile uint32_t sweep_index;
    volatile uint32_t block_index;
    volatile uint32_t sample_index;
    volatile uint32_t requested_blocks;
    volatile uint32_t samples_per_block;
    volatile uint32_t start_tick_ms;
    volatile uint32_t phase_tick_ms;
    volatile uint32_t last_tick_ms;
    volatile uint32_t success_count;
    volatile uint32_t failure_count;
    volatile uint32_t bias_apply_fail_count;
    volatile uint32_t adc_drdy_timeout_count;
    volatile uint32_t adc_read_fail_count;
    volatile uint32_t discard_fail_count;
    volatile uint32_t pilot_bias_apply_fail_count;
    volatile uint32_t finalize_fail_count;
    volatile uint32_t point_overflow_fail_count;
} dpmzm_scan_trace_t;

extern volatile dpmzm_scan_trace_t g_dpmzm_scan_trace;

const char *dpmzm_scan_stage_name(dpmzm_scan_stage_t stage);
const char *dpmzm_scan_target_name(dpmzm_scan_target_t target);
const char *dpmzm_scan_pilot_mode_name(dpmzm_scan_pilot_mode_t mode);
const char *dpmzm_scan_trace_phase_name(dpmzm_scan_trace_phase_t phase);
const char *dpmzm_scan_trace_error_name(dpmzm_scan_trace_error_t error);
void dpmzm_scan_trace_snapshot(dpmzm_scan_trace_t *out);

bool dpmzm_scan_run(const dpmzm_scan_request_t *req,
                    dpmzm_scan_summary_t *summary_out);
bool dpmzm_scan_run_collect(const dpmzm_scan_request_t *req,
                            dpmzm_scan_summary_t *summary_out,
                            dpmzm_scan_point_t *points,
                            uint32_t point_capacity,
                            uint32_t *point_count_out);
bool dpmzm_scan_measure_point(const dpmzm_scan_request_t *req,
                              float sweep_value,
                              dpmzm_measurement_t *out);

#endif /* CTRL_SCAN_DPMZM_H */
