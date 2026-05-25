#include "ctrl_scan_dpmzm.h"
#include "ctrl_dpmzm_dsp.h"
#include "drv_ads131m02.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include <float.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define DPMZM_SCAN_DRDY_TIMEOUT_MS 5U
#define DPMZM_SCAN_DISCARD_BLOCKS_AFTER_SETTLE 1U

static float s_scan_block_ac[DPMZM_DSP_GOERTZEL_BLOCK_SIZE];
static float s_scan_block_dc[DPMZM_DSP_GOERTZEL_BLOCK_SIZE];
volatile dpmzm_scan_trace_t g_dpmzm_scan_trace;
static dpmzm_scan_trace_error_t s_last_sample_error = DPMZM_SCAN_TRACE_ERR_NONE;

typedef struct {
    float phase_rad;
    float phase_step_rad;
    float amplitude_v;
} dpmzm_tone_gen_t;

static void tone_gen_init(dpmzm_tone_gen_t *gen,
                          float frequency_hz,
                          float sample_rate_hz,
                          float amplitude_v)
{
    if (gen == NULL) {
        return;
    }

    gen->phase_rad = 0.0f;
    gen->phase_step_rad = (2.0f * M_PI * frequency_hz) / sample_rate_hz;
    gen->amplitude_v = amplitude_v;
}

static void tone_gen_reset(dpmzm_tone_gen_t *gen)
{
    if (gen == NULL) {
        return;
    }

    gen->phase_rad = 0.0f;
}

static float tone_gen_next(dpmzm_tone_gen_t *gen)
{
    float value;

    if (gen == NULL) {
        return 0.0f;
    }

    value = gen->amplitude_v * sinf(gen->phase_rad);
    gen->phase_rad += gen->phase_step_rad;
    if (gen->phase_rad >= (2.0f * M_PI)) {
        gen->phase_rad = fmodf(gen->phase_rad, 2.0f * M_PI);
    }
    return value;
}

const char *dpmzm_scan_trace_phase_name(dpmzm_scan_trace_phase_t phase)
{
    switch (phase) {
    case DPMZM_SCAN_TRACE_IDLE:
        return "idle";
    case DPMZM_SCAN_TRACE_POINT_START:
        return "point-start";
    case DPMZM_SCAN_TRACE_APPLY_BIAS:
        return "apply-bias";
    case DPMZM_SCAN_TRACE_SETTLE:
        return "settle";
    case DPMZM_SCAN_TRACE_DISCARD:
        return "discard";
    case DPMZM_SCAN_TRACE_MEASURE:
        return "measure";
    case DPMZM_SCAN_TRACE_PROCESS:
        return "process";
    case DPMZM_SCAN_TRACE_FINALIZE:
        return "finalize";
    case DPMZM_SCAN_TRACE_RESTORE:
        return "restore";
    case DPMZM_SCAN_TRACE_DONE:
        return "done";
    case DPMZM_SCAN_TRACE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *dpmzm_scan_trace_error_name(dpmzm_scan_trace_error_t error)
{
    switch (error) {
    case DPMZM_SCAN_TRACE_ERR_NONE:
        return "none";
    case DPMZM_SCAN_TRACE_ERR_BAD_ARG:
        return "bad-arg";
    case DPMZM_SCAN_TRACE_ERR_BIAS_APPLY:
        return "bias-apply";
    case DPMZM_SCAN_TRACE_ERR_ADC_DRDY_TIMEOUT:
        return "adc-drdy-timeout";
    case DPMZM_SCAN_TRACE_ERR_ADC_READ:
        return "adc-read";
    case DPMZM_SCAN_TRACE_ERR_DISCARD:
        return "discard";
    case DPMZM_SCAN_TRACE_ERR_PILOT_BIAS_APPLY:
        return "pilot-bias-apply";
    case DPMZM_SCAN_TRACE_ERR_FINALIZE:
        return "finalize";
    case DPMZM_SCAN_TRACE_ERR_POINT_OVERFLOW:
        return "point-overflow";
    default:
        return "unknown";
    }
}

void dpmzm_scan_trace_snapshot(dpmzm_scan_trace_t *out)
{
    if (out == NULL) {
        return;
    }

    out->generation = g_dpmzm_scan_trace.generation;
    out->running = g_dpmzm_scan_trace.running;
    out->phase = g_dpmzm_scan_trace.phase;
    out->last_error = g_dpmzm_scan_trace.last_error;
    out->stage = g_dpmzm_scan_trace.stage;
    out->target = g_dpmzm_scan_trace.target;
    out->pilot_mode = g_dpmzm_scan_trace.pilot_mode;
    out->dump_mode = g_dpmzm_scan_trace.dump_mode;
    out->sweep_v = g_dpmzm_scan_trace.sweep_v;
    out->base_i_v = g_dpmzm_scan_trace.base_i_v;
    out->base_q_v = g_dpmzm_scan_trace.base_q_v;
    out->base_p_v = g_dpmzm_scan_trace.base_p_v;
    out->sweep_index = g_dpmzm_scan_trace.sweep_index;
    out->block_index = g_dpmzm_scan_trace.block_index;
    out->sample_index = g_dpmzm_scan_trace.sample_index;
    out->requested_blocks = g_dpmzm_scan_trace.requested_blocks;
    out->samples_per_block = g_dpmzm_scan_trace.samples_per_block;
    out->start_tick_ms = g_dpmzm_scan_trace.start_tick_ms;
    out->phase_tick_ms = g_dpmzm_scan_trace.phase_tick_ms;
    out->last_tick_ms = g_dpmzm_scan_trace.last_tick_ms;
    out->success_count = g_dpmzm_scan_trace.success_count;
    out->failure_count = g_dpmzm_scan_trace.failure_count;
    out->bias_apply_fail_count = g_dpmzm_scan_trace.bias_apply_fail_count;
    out->adc_drdy_timeout_count = g_dpmzm_scan_trace.adc_drdy_timeout_count;
    out->adc_read_fail_count = g_dpmzm_scan_trace.adc_read_fail_count;
    out->discard_fail_count = g_dpmzm_scan_trace.discard_fail_count;
    out->pilot_bias_apply_fail_count = g_dpmzm_scan_trace.pilot_bias_apply_fail_count;
    out->finalize_fail_count = g_dpmzm_scan_trace.finalize_fail_count;
    out->point_overflow_fail_count = g_dpmzm_scan_trace.point_overflow_fail_count;
}

static void scan_trace_phase(dpmzm_scan_trace_phase_t phase)
{
    uint32_t now = HAL_GetTick();

    g_dpmzm_scan_trace.phase = phase;
    g_dpmzm_scan_trace.phase_tick_ms = now;
    g_dpmzm_scan_trace.last_tick_ms = now;
}

static void scan_trace_begin_point(const dpmzm_scan_request_t *req,
                                   float sweep_value,
                                   float base_i_v,
                                   float base_q_v,
                                   float base_p_v)
{
    uint32_t now = HAL_GetTick();

    if (req == NULL) {
        return;
    }

    g_dpmzm_scan_trace.generation++;
    g_dpmzm_scan_trace.running = true;
    g_dpmzm_scan_trace.phase = DPMZM_SCAN_TRACE_POINT_START;
    g_dpmzm_scan_trace.last_error = DPMZM_SCAN_TRACE_ERR_NONE;
    g_dpmzm_scan_trace.stage = req->stage;
    g_dpmzm_scan_trace.target = req->target;
    g_dpmzm_scan_trace.pilot_mode = req->pilot_mode;
    g_dpmzm_scan_trace.dump_mode = req->dump_mode;
    g_dpmzm_scan_trace.sweep_v = sweep_value;
    g_dpmzm_scan_trace.base_i_v = base_i_v;
    g_dpmzm_scan_trace.base_q_v = base_q_v;
    g_dpmzm_scan_trace.base_p_v = base_p_v;
    g_dpmzm_scan_trace.block_index = 0U;
    g_dpmzm_scan_trace.sample_index = 0U;
    g_dpmzm_scan_trace.requested_blocks = req->blocks;
    g_dpmzm_scan_trace.samples_per_block = DPMZM_DSP_GOERTZEL_BLOCK_SIZE;
    g_dpmzm_scan_trace.start_tick_ms = now;
    g_dpmzm_scan_trace.phase_tick_ms = now;
    g_dpmzm_scan_trace.last_tick_ms = now;
}

static void scan_trace_error(dpmzm_scan_trace_error_t error)
{
    g_dpmzm_scan_trace.last_error = error;
    g_dpmzm_scan_trace.running = false;
    g_dpmzm_scan_trace.failure_count++;

    switch (error) {
    case DPMZM_SCAN_TRACE_ERR_BIAS_APPLY:
        g_dpmzm_scan_trace.bias_apply_fail_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_ADC_DRDY_TIMEOUT:
        g_dpmzm_scan_trace.adc_drdy_timeout_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_ADC_READ:
        g_dpmzm_scan_trace.adc_read_fail_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_DISCARD:
        g_dpmzm_scan_trace.discard_fail_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_PILOT_BIAS_APPLY:
        g_dpmzm_scan_trace.pilot_bias_apply_fail_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_FINALIZE:
        g_dpmzm_scan_trace.finalize_fail_count++;
        break;
    case DPMZM_SCAN_TRACE_ERR_POINT_OVERFLOW:
        g_dpmzm_scan_trace.point_overflow_fail_count++;
        break;
    default:
        break;
    }

    scan_trace_phase(DPMZM_SCAN_TRACE_ERROR);
}

static void scan_trace_success(void)
{
    g_dpmzm_scan_trace.success_count++;
    g_dpmzm_scan_trace.running = false;
    scan_trace_phase(DPMZM_SCAN_TRACE_DONE);
}

const char *dpmzm_scan_stage_name(dpmzm_scan_stage_t stage)
{
    switch (stage) {
    case DPMZM_SCAN_STAGE_MATP:
        return "matp";
    case DPMZM_SCAN_STAGE_QTP:
        return "qtp";
    case DPMZM_SCAN_STAGE_MITP:
        return "mitp";
    default:
        return "unknown";
    }
}

const char *dpmzm_scan_target_name(dpmzm_scan_target_t target)
{
    switch (target) {
    case DPMZM_SCAN_TARGET_I:
        return "i";
    case DPMZM_SCAN_TARGET_Q:
        return "q";
    case DPMZM_SCAN_TARGET_P:
        return "p";
    default:
        return "unknown";
    }
}

const char *dpmzm_scan_pilot_mode_name(dpmzm_scan_pilot_mode_t mode)
{
    switch (mode) {
    case DPMZM_SCAN_PILOT_EXTERNAL:
        return "external";
    case DPMZM_SCAN_PILOT_ONBOARD:
        return "onboard";
    default:
        return "unknown";
    }
}

static bool scan_request_valid(const dpmzm_scan_request_t *req)
{
    if (req == NULL) {
        return false;
    }

    if (req->stage >= DPMZM_SCAN_STAGE_COUNT ||
        req->target >= DPMZM_SCAN_TARGET_COUNT ||
        req->pilot_mode >= DPMZM_SCAN_PILOT_COUNT ||
        req->dump_mode >= DPMZM_SCAN_DUMP_COUNT) {
        return false;
    }

    if (req->blocks == 0U || req->step_v == 0.0f) {
        return false;
    }

    if (req->pilot_i_freq_hz <= 0.0f || req->pilot_q_freq_hz <= 0.0f) {
        return false;
    }

    return true;
}

static float primary_metric_value(const dpmzm_scan_request_t *req,
                                  const dpmzm_measurement_t *m)
{
    if (req == NULL || m == NULL) {
        return FLT_MAX;
    }

    switch (req->stage) {
    case DPMZM_SCAN_STAGE_QTP:
        return m->mag_fsum;
    case DPMZM_SCAN_STAGE_MATP:
    case DPMZM_SCAN_STAGE_MITP:
        return (req->target == DPMZM_SCAN_TARGET_Q) ? m->mag_fq : m->mag_fi;
    default:
        return FLT_MAX;
    }
}

static uint32_t scan_measure_flags(const dpmzm_scan_request_t *req)
{
    if (req == NULL) {
        return DPMZM_MEASURE_ALL;
    }

    switch (req->stage) {
    case DPMZM_SCAN_STAGE_QTP:
        return DPMZM_MEASURE_FSUM;
    case DPMZM_SCAN_STAGE_MATP:
    case DPMZM_SCAN_STAGE_MITP:
        return (req->target == DPMZM_SCAN_TARGET_Q) ?
               DPMZM_MEASURE_FQ : DPMZM_MEASURE_FI;
    default:
        return DPMZM_MEASURE_ALL;
    }
}

static const char *primary_metric_name(const dpmzm_scan_request_t *req)
{
    if (req == NULL) {
        return "unknown";
    }

    switch (req->stage) {
    case DPMZM_SCAN_STAGE_QTP:
        return "mag_fsum_2200";
    case DPMZM_SCAN_STAGE_MATP:
    case DPMZM_SCAN_STAGE_MITP:
        return (req->target == DPMZM_SCAN_TARGET_Q) ? "mag_fQ" : "mag_fI";
    default:
        return "unknown";
    }
}

static bool wait_and_read_sample(float *sample_ac_v, float *sample_dc_v)
{
    ads131m02_sample_t sample;
    uint32_t t0;

    s_last_sample_error = DPMZM_SCAN_TRACE_ERR_NONE;
    if (sample_ac_v == NULL || sample_dc_v == NULL) {
        s_last_sample_error = DPMZM_SCAN_TRACE_ERR_BAD_ARG;
        return false;
    }

    t0 = HAL_GetTick();
    while (board_adc_drdy_read() != 0U) {
        if ((HAL_GetTick() - t0) > DPMZM_SCAN_DRDY_TIMEOUT_MS) {
            s_last_sample_error = DPMZM_SCAN_TRACE_ERR_ADC_DRDY_TIMEOUT;
            return false;
        }
    }

    if (ads131m02_read_sample(&sample) != 0 || !sample.valid) {
        s_last_sample_error = DPMZM_SCAN_TRACE_ERR_ADC_READ;
        return false;
    }

    *sample_ac_v = ads131m02_code_to_voltage(sample.ch0, ADS131M02_GAIN_1);
    *sample_dc_v = ads131m02_code_to_voltage(sample.ch1, ADS131M02_GAIN_1);
    return true;
}

static bool discard_settle_samples(uint32_t sample_count)
{
    uint32_t i;

    for (i = 0; i < sample_count; i++) {
        float sample_ac_v = 0.0f;
        float sample_dc_v = 0.0f;

        g_dpmzm_scan_trace.sample_index = i;
        if (!wait_and_read_sample(&sample_ac_v, &sample_dc_v)) {
            return false;
        }
    }

    return true;
}

static int apply_bias_triplet(uint8_t ch_i,
                              uint8_t ch_q,
                              uint8_t ch_p,
                              float vi,
                              float vq,
                              float vp)
{
    int ret_i = dac8568_set_voltage(ch_i, vi);
    int ret_q = dac8568_set_voltage(ch_q, vq);
    int ret_p = dac8568_set_voltage(ch_p, vp);

    if (ret_i != 0) {
        return ret_i;
    }
    if (ret_q != 0) {
        return ret_q;
    }
    return ret_p;
}

static int apply_scan_biases(const dpmzm_scan_request_t *req,
                             float vi,
                             float vq,
                             float vp)
{
    if (req != NULL && req->bias_apply_fn != NULL) {
        return req->bias_apply_fn(vi, vq, vp);
    }
    return apply_bias_triplet(req->bias_i_dac_channel,
                              req->bias_q_dac_channel,
                              req->bias_p_dac_channel,
                              vi,
                              vq,
                              vp);
}

static void point_base_biases(const dpmzm_scan_request_t *req,
                              float sweep_value,
                              float *vi,
                              float *vq,
                              float *vp)
{
    *vi = req->bias_i_v;
    *vq = req->bias_q_v;
    *vp = req->bias_p_v;

    switch (req->target) {
    case DPMZM_SCAN_TARGET_I:
        *vi = sweep_value;
        break;
    case DPMZM_SCAN_TARGET_Q:
        *vq = sweep_value;
        break;
    case DPMZM_SCAN_TARGET_P:
        *vp = sweep_value;
        break;
    default:
        break;
    }
}

static void print_csv_line(const dpmzm_scan_request_t *req,
                           float sweep_value,
                           float vi,
                           float vq,
                           float vp,
                           const dpmzm_measurement_t *m)
{
    printf("DPMZMCSV,%s,%s,%.6f,V,%.6f,%.6f,%.6f,%s,%lu,%.9f,%.9f,%.9f,%.9f,%.9f\r\n",
           dpmzm_scan_stage_name(req->stage),
           dpmzm_scan_target_name(req->target),
           (double)sweep_value,
           (double)vi,
           (double)vq,
           (double)vp,
           dpmzm_scan_pilot_mode_name(req->pilot_mode),
           (unsigned long)req->blocks,
           (double)m->mag_fi,
           (double)m->mag_fq,
           (double)m->mag_fdiff,
           (double)m->mag_fsum,
           (double)m->dc_mean);
}

static void print_raw_line(const dpmzm_scan_request_t *req,
                           float sweep_value,
                           float vi,
                           float vq,
                           float vp,
                           uint32_t block_id,
                           uint32_t sample_index,
                           float sample_ac_v)
{
    printf("DPMZMRAW,%s,%s,%.6f,V,%.6f,%.6f,%.6f,%s,%lu,%lu,%.9f,%u\r\n",
           dpmzm_scan_stage_name(req->stage),
           dpmzm_scan_target_name(req->target),
           (double)sweep_value,
           (double)vi,
           (double)vq,
           (double)vp,
           dpmzm_scan_pilot_mode_name(req->pilot_mode),
           (unsigned long)block_id,
           (unsigned long)sample_index,
           (double)sample_ac_v,
           (unsigned)DPMZM_DSP_SAMPLE_RATE_HZ);
}

bool dpmzm_scan_measure_point(const dpmzm_scan_request_t *req,
                              float sweep_value,
                              dpmzm_measurement_t *out)
{
    dpmzm_measure_ctx_t measure_ctx;
    dpmzm_tone_gen_t tone_i;
    dpmzm_tone_gen_t tone_q;
    dpmzm_measurement_t block_result;
    float sum_fi = 0.0f;
    float sum_fq = 0.0f;
    float sum_fdiff = 0.0f;
    float sum_fsum = 0.0f;
    float sum_dc = 0.0f;
    float base_vi = 0.0f;
    float base_vq = 0.0f;
    float base_vp = 0.0f;
    uint32_t b;

    if (req == NULL || out == NULL) {
        scan_trace_error(DPMZM_SCAN_TRACE_ERR_BAD_ARG);
        return false;
    }

    point_base_biases(req, sweep_value, &base_vi, &base_vq, &base_vp);
    scan_trace_begin_point(req, sweep_value, base_vi, base_vq, base_vp);
    scan_trace_phase(DPMZM_SCAN_TRACE_APPLY_BIAS);
    if (apply_scan_biases(req, base_vi, base_vq, base_vp) != 0) {
        scan_trace_error(DPMZM_SCAN_TRACE_ERR_BIAS_APPLY);
        printf("[dpmzm][scan] ERROR: bias apply failed at %s-%s sweep=%+.3fV\r\n",
               dpmzm_scan_stage_name(req->stage),
               dpmzm_scan_target_name(req->target),
               (double)sweep_value);
        return false;
    }

    scan_trace_phase(DPMZM_SCAN_TRACE_SETTLE);
    board_delay_ms(req->settle_ms);
    /*
     * The first coherent block after a bias step can contain DAC/analog/ADC
     * settling residue. Drop it so each reported scan point is computed from
     * steady-state samples only.
     */
    scan_trace_phase(DPMZM_SCAN_TRACE_DISCARD);
    if (!discard_settle_samples(DPMZM_DSP_GOERTZEL_BLOCK_SIZE *
                                DPMZM_SCAN_DISCARD_BLOCKS_AFTER_SETTLE)) {
        scan_trace_error(s_last_sample_error == DPMZM_SCAN_TRACE_ERR_NONE ?
                         DPMZM_SCAN_TRACE_ERR_DISCARD :
                         s_last_sample_error);
        printf("[dpmzm][scan] ERROR: ADC discard failed at %s-%s sweep=%+.3fV\r\n",
               dpmzm_scan_stage_name(req->stage),
               dpmzm_scan_target_name(req->target),
               (double)sweep_value);
        return false;
    }

    dpmzm_measure_init_select(&measure_ctx,
                              req->pilot_i_freq_hz,
                              req->pilot_q_freq_hz,
                              (float)DPMZM_DSP_SAMPLE_RATE_HZ,
                              DPMZM_DSP_GOERTZEL_BLOCK_SIZE,
                              scan_measure_flags(req));
    if (req->pilot_mode == DPMZM_SCAN_PILOT_ONBOARD &&
        !req->continuous_onboard_pilot) {
        tone_gen_init(&tone_i,
                      req->pilot_i_freq_hz,
                      (float)DPMZM_DSP_SAMPLE_RATE_HZ,
                      req->pilot_i_amp_v);
        tone_gen_init(&tone_q,
                      req->pilot_q_freq_hz,
                      (float)DPMZM_DSP_SAMPLE_RATE_HZ,
                      req->pilot_q_amp_v);
    }

    for (b = 0; b < req->blocks; b++) {
        uint32_t s;

        g_dpmzm_scan_trace.block_index = b;
        g_dpmzm_scan_trace.sample_index = 0U;
        dpmzm_measure_reset(&measure_ctx);
        if (req->pilot_mode == DPMZM_SCAN_PILOT_ONBOARD &&
            !req->continuous_onboard_pilot) {
            tone_gen_reset(&tone_i);
            tone_gen_reset(&tone_q);
        }

        scan_trace_phase(DPMZM_SCAN_TRACE_MEASURE);
        for (s = 0; s < DPMZM_DSP_GOERTZEL_BLOCK_SIZE; s++) {
            float sample_ac_v = 0.0f;
            float sample_dc_v = 0.0f;

            g_dpmzm_scan_trace.block_index = b;
            g_dpmzm_scan_trace.sample_index = s;
            /*
             * Onboard-pilot scans now prefer the continuously running TIM6
             * generator prepared by app_dpmzm_scan_begin(). This avoids
             * rewriting DAC outputs immediately before each ADC sample, which
             * was injecting large step-update artifacts into the observed
             * spectrum. The old foreground tone generator remains available as
             * a fallback when continuous_onboard_pilot is disabled.
             */
            if (req->pilot_mode == DPMZM_SCAN_PILOT_ONBOARD &&
                !req->continuous_onboard_pilot) {
                float drive_vi = base_vi;
                float drive_vq = base_vq;

                drive_vi += tone_gen_next(&tone_i);
                drive_vq += tone_gen_next(&tone_q);

                if (apply_scan_biases(req, drive_vi, drive_vq, base_vp) != 0) {
                    scan_trace_error(DPMZM_SCAN_TRACE_ERR_PILOT_BIAS_APPLY);
                    printf("[dpmzm][scan] ERROR: pilot bias apply failed at %s-%s sweep=%+.3fV block=%lu sample=%lu\r\n",
                           dpmzm_scan_stage_name(req->stage),
                           dpmzm_scan_target_name(req->target),
                           (double)sweep_value,
                           (unsigned long)b,
                           (unsigned long)s);
                    return false;
                }
            }

            if (!wait_and_read_sample(&sample_ac_v, &sample_dc_v)) {
                scan_trace_error(s_last_sample_error == DPMZM_SCAN_TRACE_ERR_NONE ?
                                 DPMZM_SCAN_TRACE_ERR_ADC_READ :
                                 s_last_sample_error);
                printf("[dpmzm][scan] ERROR: ADC read failed at %s-%s sweep=%+.3fV block=%lu sample=%lu\r\n",
                       dpmzm_scan_stage_name(req->stage),
                       dpmzm_scan_target_name(req->target),
                       (double)sweep_value,
                       (unsigned long)b,
                       (unsigned long)s);
                return false;
            }

            s_scan_block_ac[s] = sample_ac_v;
            s_scan_block_dc[s] = sample_dc_v;
        }

        scan_trace_phase(DPMZM_SCAN_TRACE_PROCESS);
        for (s = 0; s < DPMZM_DSP_GOERTZEL_BLOCK_SIZE; s++) {
            if (req->dump_mode == DPMZM_SCAN_DUMP_RAW ||
                req->dump_mode == DPMZM_SCAN_DUMP_BOTH) {
                print_raw_line(req,
                               sweep_value,
                               base_vi,
                               base_vq,
                               base_vp,
                               b,
                               s,
                               s_scan_block_ac[s]);
            }

            dpmzm_measure_process_sample(&measure_ctx,
                                         s_scan_block_ac[s],
                                         s_scan_block_dc[s]);
        }

        scan_trace_phase(DPMZM_SCAN_TRACE_FINALIZE);
        if (!dpmzm_measure_finalize(&measure_ctx, &block_result)) {
            scan_trace_error(DPMZM_SCAN_TRACE_ERR_FINALIZE);
            printf("[dpmzm][scan] ERROR: measure finalize failed at %s-%s sweep=%+.3fV block=%lu\r\n",
                   dpmzm_scan_stage_name(req->stage),
                   dpmzm_scan_target_name(req->target),
                   (double)sweep_value,
                   (unsigned long)b);
            return false;
        }

        sum_fi += block_result.mag_fi;
        sum_fq += block_result.mag_fq;
        sum_fdiff += block_result.mag_fdiff;
        sum_fsum += block_result.mag_fsum;
        sum_dc += block_result.dc_mean;
    }

    out->mag_fi = sum_fi / (float)req->blocks;
    out->mag_fq = sum_fq / (float)req->blocks;
    out->mag_fdiff = sum_fdiff / (float)req->blocks;
    out->mag_fsum = sum_fsum / (float)req->blocks;
    out->dc_mean = sum_dc / (float)req->blocks;
    out->sample_count = req->blocks * DPMZM_DSP_GOERTZEL_BLOCK_SIZE;

    scan_trace_phase(DPMZM_SCAN_TRACE_RESTORE);
    (void)apply_scan_biases(req, base_vi, base_vq, base_vp);
    scan_trace_success();
    return true;
}

bool dpmzm_scan_run(const dpmzm_scan_request_t *req,
                    dpmzm_scan_summary_t *summary_out)
{
    return dpmzm_scan_run_collect(req, summary_out, NULL, 0U, NULL);
}

bool dpmzm_scan_run_collect(const dpmzm_scan_request_t *req,
                            dpmzm_scan_summary_t *summary_out,
                            dpmzm_scan_point_t *points,
                            uint32_t point_capacity,
                            uint32_t *point_count_out)
{
    dpmzm_scan_summary_t local_summary;
    float sweep = 0.0f;
    bool first_point = true;
    float best_metric = FLT_MAX;
    float second_metric = FLT_MAX;
    float step_sign;
    uint32_t point_count = 0U;

    if (!scan_request_valid(req)) {
        return false;
    }
    if (point_count_out != NULL) {
        *point_count_out = 0U;
    }

    local_summary.valid = false;
    local_summary.primary_metric_name = primary_metric_name(req);
    local_summary.best_sweep_value = 0.0f;
    local_summary.best_metric_value = 0.0f;
    local_summary.secondary_best_sweep_value = 0.0f;

    step_sign = (req->stop_v >= req->start_v) ? fabsf(req->step_v) : -fabsf(req->step_v);

    for (sweep = req->start_v;
         (step_sign > 0.0f) ? (sweep <= req->stop_v + 1e-6f) : (sweep >= req->stop_v - 1e-6f);
         sweep += step_sign) {
        dpmzm_measurement_t m;
        float base_vi = 0.0f;
        float base_vq = 0.0f;
        float base_vp = 0.0f;
        float primary;

        g_dpmzm_scan_trace.sweep_index = point_count;
        if (!dpmzm_scan_measure_point(req, sweep, &m)) {
            return false;
        }

        point_base_biases(req, sweep, &base_vi, &base_vq, &base_vp);
        if (req->dump_mode == DPMZM_SCAN_DUMP_METRICS ||
            req->dump_mode == DPMZM_SCAN_DUMP_BOTH) {
            print_csv_line(req, sweep, base_vi, base_vq, base_vp, &m);
        }
        if (points != NULL) {
            if (point_count >= point_capacity) {
                scan_trace_error(DPMZM_SCAN_TRACE_ERR_POINT_OVERFLOW);
                return false;
            }
            points[point_count].sweep_v = sweep;
            points[point_count].mag_fi = m.mag_fi;
            points[point_count].mag_fq = m.mag_fq;
            points[point_count].mag_fdiff = m.mag_fdiff;
            points[point_count].mag_fsum = m.mag_fsum;
            points[point_count].dc_mean = m.dc_mean;
        }
        point_count++;

        primary = primary_metric_value(req, &m);
        if (first_point || primary < best_metric) {
            second_metric = best_metric;
            local_summary.secondary_best_sweep_value = local_summary.best_sweep_value;
            best_metric = primary;
            local_summary.best_sweep_value = sweep;
            local_summary.best_metric_value = primary;
            local_summary.valid = true;
            first_point = false;
        } else if (primary < second_metric) {
            second_metric = primary;
            local_summary.secondary_best_sweep_value = sweep;
        }
    }

    printf("DPMZMSUM,%s,%s,%s,%.6f,%.9f,%.6f\r\n",
           dpmzm_scan_stage_name(req->stage),
           dpmzm_scan_target_name(req->target),
           local_summary.primary_metric_name,
           (double)local_summary.best_sweep_value,
           (double)local_summary.best_metric_value,
           (double)local_summary.secondary_best_sweep_value);

    if (summary_out != NULL) {
        *summary_out = local_summary;
    }
    if (point_count_out != NULL) {
        *point_count_out = point_count;
    }

    return local_summary.valid;
}
