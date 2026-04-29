#ifndef CTRL_LOCK_DPMZM_H
#define CTRL_LOCK_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "ctrl_scan_dpmzm.h"

typedef enum {
    DPMZM_LOCK_AXIS_I = 0,
    DPMZM_LOCK_AXIS_Q,
    DPMZM_LOCK_AXIS_P,
    DPMZM_LOCK_AXIS_COUNT
} dpmzm_lock_axis_t;

typedef enum {
    DPMZM_LOCK_IDLE = 0,
    DPMZM_LOCK_RUNNING,
    DPMZM_LOCK_STOPPED,
    DPMZM_LOCK_FAULT
} dpmzm_lock_state_t;

typedef enum {
    DPMZM_LOCK_OK = 0,
    DPMZM_LOCK_ERR_BAD_ARG,
    DPMZM_LOCK_ERR_MEASURE,
    DPMZM_LOCK_ERR_BAD_METRIC
} dpmzm_lock_error_t;

typedef struct {
    dpmzm_scan_request_t scan_template;
    float delta_v;
    float gain_v;
    float max_step_v;
    float deadband_rel;
    float min_bias_v;
    float max_bias_v;
    uint32_t loop_interval_ms;
} dpmzm_lock_request_t;

typedef struct {
    bool valid;
    dpmzm_lock_axis_t axis;
    float center_v;
    float plus_v;
    float minus_v;
    float metric_plus;
    float metric_minus;
    float metric_plus_dbm;
    float metric_minus_dbm;
    float error;
    float dc_plus_v;
    float dc_minus_v;
    const char *direction;
    dpmzm_lock_error_t error_code;
} dpmzm_lock_probe_result_t;

typedef struct {
    dpmzm_lock_probe_result_t probe;
    float requested_step_v;
    float applied_step_v;
    float new_bias_v;
    bool held_by_deadband;
    bool clamped;
    dpmzm_lock_error_t error_code;
} dpmzm_lock_step_result_t;

typedef struct {
    dpmzm_lock_state_t state;
    dpmzm_lock_error_t error;
    bool enabled;
    uint32_t update_count;
    uint32_t hold_count;
    uint32_t fault_count;
    uint32_t sequence_index;
    dpmzm_lock_axis_t last_axis;
    float last_error_value;
    float last_step_v;
    float last_bias_v;
} dpmzm_lock_context_t;

void dpmzm_lock_init(void);
const dpmzm_lock_context_t *dpmzm_lock_get_context(void);
const char *dpmzm_lock_axis_name(dpmzm_lock_axis_t axis);
const char *dpmzm_lock_state_name(dpmzm_lock_state_t state);
const char *dpmzm_lock_error_name(dpmzm_lock_error_t error);
bool dpmzm_lock_axis_from_char(char c, dpmzm_lock_axis_t *axis_out);

void dpmzm_lock_start(void);
void dpmzm_lock_stop(void);
dpmzm_lock_axis_t dpmzm_lock_next_axis(void);
void dpmzm_lock_record_step(const dpmzm_lock_step_result_t *result);
void dpmzm_lock_record_fault(dpmzm_lock_error_t error);

bool dpmzm_lock_probe(const dpmzm_lock_request_t *req,
                      dpmzm_lock_axis_t axis,
                      dpmzm_lock_probe_result_t *out);
bool dpmzm_lock_step(const dpmzm_lock_request_t *req,
                     dpmzm_lock_axis_t axis,
                     dpmzm_lock_step_result_t *out);

void dpmzm_lock_print_probe_result(const dpmzm_lock_probe_result_t *result);
void dpmzm_lock_print_step_result(const dpmzm_lock_step_result_t *result);
void dpmzm_lock_print_status(void);

#endif /* CTRL_LOCK_DPMZM_H */
