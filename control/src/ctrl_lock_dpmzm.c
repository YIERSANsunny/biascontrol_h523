#include "ctrl_lock_dpmzm.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DPMZM_LOCK_DBM_FLOOR_MW 1e-15f

static dpmzm_lock_context_t s_lock_ctx;
static const dpmzm_lock_axis_t s_lock_sequence[] = {
    DPMZM_LOCK_AXIS_P,
    DPMZM_LOCK_AXIS_I,
    DPMZM_LOCK_AXIS_P,
    DPMZM_LOCK_AXIS_Q,
    DPMZM_LOCK_AXIS_P
};

static float clampf_local(float value, float min_v, float max_v)
{
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

static float metric_to_dbm(float vpeak)
{
    const float vrms = vpeak * 0.70710678f;
    float power_mw = ((vrms * vrms) / 50.0f) * 1000.0f;

    if (power_mw < DPMZM_LOCK_DBM_FLOOR_MW) {
        power_mw = DPMZM_LOCK_DBM_FLOOR_MW;
    }
    return 10.0f * log10f(power_mw);
}

static float axis_center_v(const dpmzm_scan_request_t *req,
                           dpmzm_lock_axis_t axis)
{
    if (req == NULL) {
        return 0.0f;
    }

    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return req->bias_i_v;
    case DPMZM_LOCK_AXIS_Q:
        return req->bias_q_v;
    case DPMZM_LOCK_AXIS_P:
        return req->bias_p_v;
    default:
        return 0.0f;
    }
}

static void set_axis_stage_target(dpmzm_scan_request_t *req,
                                  dpmzm_lock_axis_t axis)
{
    if (req == NULL) {
        return;
    }

    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        req->stage = DPMZM_SCAN_STAGE_MITP;
        req->target = DPMZM_SCAN_TARGET_I;
        break;
    case DPMZM_LOCK_AXIS_Q:
        req->stage = DPMZM_SCAN_STAGE_MITP;
        req->target = DPMZM_SCAN_TARGET_Q;
        break;
    case DPMZM_LOCK_AXIS_P:
        req->stage = DPMZM_SCAN_STAGE_QTP;
        req->target = DPMZM_SCAN_TARGET_P;
        break;
    default:
        break;
    }
}

static float metric_for_axis(dpmzm_lock_axis_t axis,
                             const dpmzm_measurement_t *m)
{
    if (m == NULL) {
        return FLT_MAX;
    }

    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return m->mag_fi;
    case DPMZM_LOCK_AXIS_Q:
        return m->mag_fq;
    case DPMZM_LOCK_AXIS_P:
        return m->mag_fsum;
    default:
        return FLT_MAX;
    }
}

static bool apply_center_biases(const dpmzm_scan_request_t *req)
{
    if (req == NULL) {
        return false;
    }
    if (req->bias_apply_fn == NULL) {
        return true;
    }
    return req->bias_apply_fn(req->bias_i_v, req->bias_q_v, req->bias_p_v) == 0;
}

static bool request_valid(const dpmzm_lock_request_t *req)
{
    if (req == NULL) {
        return false;
    }
    if (req->delta_v <= 0.0f ||
        req->gain_v <= 0.0f ||
        req->max_step_v <= 0.0f ||
        req->deadband_rel < 0.0f ||
        req->scan_template.blocks == 0U ||
        req->scan_template.pilot_i_freq_hz <= 0.0f ||
        req->scan_template.pilot_q_freq_hz <= 0.0f ||
        req->max_bias_v <= req->min_bias_v) {
        return false;
    }
    return true;
}

void dpmzm_lock_init(void)
{
    memset(&s_lock_ctx, 0, sizeof(s_lock_ctx));
    s_lock_ctx.state = DPMZM_LOCK_IDLE;
    s_lock_ctx.error = DPMZM_LOCK_OK;
}

const dpmzm_lock_context_t *dpmzm_lock_get_context(void)
{
    return &s_lock_ctx;
}

const char *dpmzm_lock_axis_name(dpmzm_lock_axis_t axis)
{
    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return "i";
    case DPMZM_LOCK_AXIS_Q:
        return "q";
    case DPMZM_LOCK_AXIS_P:
        return "p";
    default:
        return "unknown";
    }
}

const char *dpmzm_lock_state_name(dpmzm_lock_state_t state)
{
    switch (state) {
    case DPMZM_LOCK_IDLE:
        return "IDLE";
    case DPMZM_LOCK_RUNNING:
        return "RUNNING";
    case DPMZM_LOCK_STOPPED:
        return "STOPPED";
    case DPMZM_LOCK_FAULT:
        return "FAULT";
    default:
        return "UNKNOWN";
    }
}

const char *dpmzm_lock_error_name(dpmzm_lock_error_t error)
{
    switch (error) {
    case DPMZM_LOCK_OK:
        return "OK";
    case DPMZM_LOCK_ERR_BAD_ARG:
        return "BAD_ARG";
    case DPMZM_LOCK_ERR_MEASURE:
        return "MEASURE";
    case DPMZM_LOCK_ERR_BAD_METRIC:
        return "BAD_METRIC";
    default:
        return "UNKNOWN";
    }
}

bool dpmzm_lock_axis_from_char(char c, dpmzm_lock_axis_t *axis_out)
{
    if (axis_out == NULL) {
        return false;
    }

    switch (c) {
    case 'i':
    case 'I':
        *axis_out = DPMZM_LOCK_AXIS_I;
        return true;
    case 'q':
    case 'Q':
        *axis_out = DPMZM_LOCK_AXIS_Q;
        return true;
    case 'p':
    case 'P':
        *axis_out = DPMZM_LOCK_AXIS_P;
        return true;
    default:
        return false;
    }
}

void dpmzm_lock_start(void)
{
    s_lock_ctx.enabled = true;
    s_lock_ctx.state = DPMZM_LOCK_RUNNING;
    s_lock_ctx.error = DPMZM_LOCK_OK;
    s_lock_ctx.sequence_index = 0U;
}

void dpmzm_lock_stop(void)
{
    s_lock_ctx.enabled = false;
    if (s_lock_ctx.state != DPMZM_LOCK_FAULT) {
        s_lock_ctx.state = DPMZM_LOCK_STOPPED;
    }
}

dpmzm_lock_axis_t dpmzm_lock_next_axis(void)
{
    dpmzm_lock_axis_t axis;
    uint32_t seq_len = (uint32_t)(sizeof(s_lock_sequence) / sizeof(s_lock_sequence[0]));

    if (seq_len == 0U) {
        return DPMZM_LOCK_AXIS_P;
    }

    axis = s_lock_sequence[s_lock_ctx.sequence_index % seq_len];
    s_lock_ctx.sequence_index = (s_lock_ctx.sequence_index + 1U) % seq_len;
    return axis;
}

void dpmzm_lock_record_step(const dpmzm_lock_step_result_t *result)
{
    if (result == NULL || !result->probe.valid) {
        return;
    }

    s_lock_ctx.error = DPMZM_LOCK_OK;
    s_lock_ctx.last_axis = result->probe.axis;
    s_lock_ctx.last_error_value = result->probe.error;
    s_lock_ctx.last_step_v = result->applied_step_v;
    s_lock_ctx.last_bias_v = result->new_bias_v;
    if (result->held_by_deadband) {
        s_lock_ctx.hold_count++;
    } else {
        s_lock_ctx.update_count++;
    }
}

void dpmzm_lock_record_fault(dpmzm_lock_error_t error)
{
    s_lock_ctx.error = error;
    s_lock_ctx.state = DPMZM_LOCK_FAULT;
    s_lock_ctx.enabled = false;
    s_lock_ctx.fault_count++;
}

bool dpmzm_lock_probe(const dpmzm_lock_request_t *req,
                      dpmzm_lock_axis_t axis,
                      dpmzm_lock_probe_result_t *out)
{
    dpmzm_scan_request_t point_req;
    dpmzm_measurement_t plus_m;
    dpmzm_measurement_t minus_m;
    float center_v;
    float denom;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->axis = axis;
    out->error_code = DPMZM_LOCK_OK;

    if (!request_valid(req) || axis >= DPMZM_LOCK_AXIS_COUNT) {
        out->error_code = DPMZM_LOCK_ERR_BAD_ARG;
        return false;
    }

    point_req = req->scan_template;
    point_req.dump_mode = DPMZM_SCAN_DUMP_METRICS;
    set_axis_stage_target(&point_req, axis);

    center_v = axis_center_v(&point_req, axis);
    out->center_v = center_v;
    out->plus_v = clampf_local(center_v + req->delta_v,
                               req->min_bias_v,
                               req->max_bias_v);
    out->minus_v = clampf_local(center_v - req->delta_v,
                                req->min_bias_v,
                                req->max_bias_v);

    if (!dpmzm_scan_measure_point(&point_req, out->plus_v, &plus_m)) {
        out->error_code = DPMZM_LOCK_ERR_MEASURE;
        (void)apply_center_biases(&point_req);
        return false;
    }
    if (!dpmzm_scan_measure_point(&point_req, out->minus_v, &minus_m)) {
        out->error_code = DPMZM_LOCK_ERR_MEASURE;
        (void)apply_center_biases(&point_req);
        return false;
    }

    (void)apply_center_biases(&point_req);

    out->metric_plus = metric_for_axis(axis, &plus_m);
    out->metric_minus = metric_for_axis(axis, &minus_m);
    out->metric_plus_dbm = metric_to_dbm(out->metric_plus);
    out->metric_minus_dbm = metric_to_dbm(out->metric_minus);
    out->dc_plus_v = plus_m.dc_mean;
    out->dc_minus_v = minus_m.dc_mean;

    if (!isfinite(out->metric_plus) ||
        !isfinite(out->metric_minus) ||
        out->metric_plus < 0.0f ||
        out->metric_minus < 0.0f) {
        out->error_code = DPMZM_LOCK_ERR_BAD_METRIC;
        return false;
    }

    denom = out->metric_plus + out->metric_minus + 1e-12f;
    out->error = (out->metric_plus - out->metric_minus) / denom;
    if (fabsf(out->error) < req->deadband_rel) {
        out->direction = "hold";
    } else if (out->error > 0.0f) {
        out->direction = "negative";
    } else {
        out->direction = "positive";
    }

    out->valid = true;
    return true;
}

bool dpmzm_lock_step(const dpmzm_lock_request_t *req,
                     dpmzm_lock_axis_t axis,
                     dpmzm_lock_step_result_t *out)
{
    float raw_step;
    float center_v;
    float new_bias_v;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->error_code = DPMZM_LOCK_OK;

    if (!dpmzm_lock_probe(req, axis, &out->probe)) {
        out->error_code = out->probe.error_code;
        return false;
    }

    center_v = out->probe.center_v;
    if (fabsf(out->probe.error) < req->deadband_rel) {
        raw_step = 0.0f;
        out->held_by_deadband = true;
    } else {
        raw_step = -req->gain_v * out->probe.error;
    }

    out->requested_step_v = raw_step;
    out->applied_step_v = clampf_local(raw_step,
                                       -fabsf(req->max_step_v),
                                       fabsf(req->max_step_v));
    new_bias_v = center_v + out->applied_step_v;
    out->new_bias_v = clampf_local(new_bias_v, req->min_bias_v, req->max_bias_v);
    out->clamped = fabsf(out->new_bias_v - new_bias_v) > 1e-6f;
    return true;
}

void dpmzm_lock_print_probe_result(const dpmzm_lock_probe_result_t *result)
{
    if (result == NULL) {
        printf("[dpmzm][lock] no probe result\r\n");
        return;
    }

    printf("[dpmzm][lock] probe axis=%s valid=%s error=%s\r\n",
           dpmzm_lock_axis_name(result->axis),
           result->valid ? "yes" : "no",
           dpmzm_lock_error_name(result->error_code));
    printf("  center: %+.3fV plus=%+.3fV minus=%+.3fV\r\n",
           (double)result->center_v,
           (double)result->plus_v,
           (double)result->minus_v);
    printf("  metric+: %.9f (%.2f dBm) dc=%+.6fV\r\n",
           (double)result->metric_plus,
           (double)result->metric_plus_dbm,
           (double)result->dc_plus_v);
    printf("  metric-: %.9f (%.2f dBm) dc=%+.6fV\r\n",
           (double)result->metric_minus,
           (double)result->metric_minus_dbm,
           (double)result->dc_minus_v);
    printf("  e:       %+.6f direction=%s\r\n",
           (double)result->error,
           result->direction != NULL ? result->direction : "unknown");
}

void dpmzm_lock_print_step_result(const dpmzm_lock_step_result_t *result)
{
    if (result == NULL) {
        printf("[dpmzm][lock] no step result\r\n");
        return;
    }

    dpmzm_lock_print_probe_result(&result->probe);
    printf("[dpmzm][lock] step axis=%s requested=%+.6fV applied=%+.6fV new=%+.6fV hold=%s clamp=%s\r\n",
           dpmzm_lock_axis_name(result->probe.axis),
           (double)result->requested_step_v,
           (double)result->applied_step_v,
           (double)result->new_bias_v,
           result->held_by_deadband ? "yes" : "no",
           result->clamped ? "yes" : "no");
}

void dpmzm_lock_print_status(void)
{
    printf("[dpmzm][lock] status\r\n");
    printf("  state:       %s\r\n", dpmzm_lock_state_name(s_lock_ctx.state));
    printf("  enabled:     %s\r\n", s_lock_ctx.enabled ? "yes" : "no");
    printf("  error:       %s\r\n", dpmzm_lock_error_name(s_lock_ctx.error));
    printf("  updates:     %lu\r\n", (unsigned long)s_lock_ctx.update_count);
    printf("  holds:       %lu\r\n", (unsigned long)s_lock_ctx.hold_count);
    printf("  faults:      %lu\r\n", (unsigned long)s_lock_ctx.fault_count);
    printf("  last axis:   %s\r\n", dpmzm_lock_axis_name(s_lock_ctx.last_axis));
    printf("  last error:  %+.6f\r\n", (double)s_lock_ctx.last_error_value);
    printf("  last step:   %+.6fV\r\n", (double)s_lock_ctx.last_step_v);
    printf("  last bias:   %+.6fV\r\n", (double)s_lock_ctx.last_bias_v);
}
