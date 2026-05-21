#include "app_main.h"
#include "app_main_dpmzm.h"
#include "app_uart.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "drv_ads131m02.h"
#include "dsp_goertzel.h"
#include "dsp_pilot_gen.h"
#include "dsp_types.h"
#include "ctrl_modulator_mzm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define SCAN_MAX_STEPS 201
#define CAL_VERIFY_BLOCKS 5
#define SCAN_INITIAL_SETTLE_MS 1000
#define CAL_VERIFY_SETTLE_MS SCAN_INITIAL_SETTLE_MS

/* Large scan buffers live in static storage to avoid blowing the MCU stack. */
static float s_scan_bias_buf[SCAN_MAX_STEPS];
static float s_scan_h1_mag_buf[SCAN_MAX_STEPS];
static float s_scan_h1_signed_buf[SCAN_MAX_STEPS];
static float s_scan_h2_signed_buf[SCAN_MAX_STEPS];
static float s_scan_dc_buf[SCAN_MAX_STEPS];
static float s_fit_sin_basis[SCAN_MAX_STEPS];
static float s_fit_cos_basis[SCAN_MAX_STEPS];
static float s_fit_phi_basis[SCAN_MAX_STEPS];
static float s_fit_sin_phi_basis[SCAN_MAX_STEPS];
static float s_fit_cos_phi_basis[SCAN_MAX_STEPS];
static float s_fit_h2_smooth_buf[SCAN_MAX_STEPS];

static float robust_meanf_local(const float *vals, int n)
{
    float tmp[20];
    if (n <= 0) {
        return 0.0f;
    }
    for (int i = 0; i < n; i++) {
        tmp[i] = vals[i];
    }
    for (int i = 1; i < n; i++) {
        float key = tmp[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > key) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }
    if (n == 1) return tmp[0];
    if (n == 2) return 0.5f * (tmp[0] + tmp[1]);
    if (n == 3) return tmp[1];
    if (n == 4) return 0.5f * (tmp[1] + tmp[2]);
    {
        float sum = 0.0f;
        for (int i = 1; i < n - 1; i++) {
            sum += tmp[i];
        }
        return sum / (float)(n - 2);
    }
}

typedef struct {
    bool valid;
    float vpi_v;
    float null_v;
    float peak_v;
    float quad_pos_v;
    float quad_neg_v;
    /* Harmonic-axis calibration for the phase-vector controller */
    float h1_offset;
    float h2_offset;
    float h1_axis;
    float h2_axis;
    float h1_axis_sign;
    float h2_axis_sign;
    float pilot_cal_v;
    bool  harmonics_valid;
    bool  affine_valid;
    float affine_o1;
    float affine_o2;
    float affine_m11;
    float affine_m12;
    float affine_m21;
    float affine_m22;
    float dc_null_v;     /**< TIA DC voltage at extinction */
    float dc_peak_v;     /**< TIA DC voltage at maximum transmission */
    bool  dc_cal_valid;  /**< True when DC calibration values are available */
    float obs_y_quad;    /**< Observer obs_y at QUAD (affine-model residual = b_obs) */
    bool  obs_y_quad_valid;
} bias_cal_result_t;

/* ========================================================================= */
/*  Application context (singleton)                                          */
/* ========================================================================= */

static app_context_t ctx;

typedef enum {
    APP_RUNTIME_MODE_MZM = 0,
    APP_RUNTIME_MODE_DPMZM,
    APP_RUNTIME_MODE_IDLE,
} app_runtime_mode_t;

static app_runtime_mode_t s_runtime_mode = APP_RUNTIME_MODE_MZM;

static const char *runtime_mode_name(app_runtime_mode_t mode)
{
    switch (mode) {
    case APP_RUNTIME_MODE_MZM:
        return "mzm";
    case APP_RUNTIME_MODE_DPMZM:
        return "dpmzm";
    case APP_RUNTIME_MODE_IDLE:
        return "idle";
    default:
        return "unknown";
    }
}

/* ========================================================================= */
/*  State name table                                                         */
/* ========================================================================= */

static const char *state_names[] = {
    [APP_STATE_INIT]        = "INIT",
    [APP_STATE_HW_SELFTEST] = "SELFTEST",
    [APP_STATE_IDLE]        = "IDLE",
    [APP_STATE_SWEEPING]    = "SWEEPING",
    [APP_STATE_LOCKING]     = "LOCKING",
    [APP_STATE_LOCKED]      = "LOCKED",
    [APP_STATE_FAULT]       = "FAULT",
};

const char *app_state_name(app_state_t state)
{
    if (state < APP_STATE_COUNT) {
        return state_names[state];
    }
    return "UNKNOWN";
}

/* ========================================================================= */
/*  State transition helper                                                  */
/* ========================================================================= */

static void transition_to(app_state_t new_state)
{
    ctx.state = new_state;
    ctx.tick_ms = HAL_GetTick();
    ctx.state_enter_ms = ctx.tick_ms;
}

static void stop_mzm_runtime(void)
{
    if (ctx.state == APP_STATE_INIT ||
        ctx.state == APP_STATE_HW_SELFTEST ||
        ctx.state == APP_STATE_FAULT) {
        return;
    }

    bias_ctrl_stop(&ctx.bias_ctrl);
    ads131m02_stop_continuous();
    transition_to(APP_STATE_IDLE);
}

static void stop_dpmzm_runtime(void)
{
    const app_dpmzm_context_t *dpmzm_ctx = app_dpmzm_get_context();

    if (dpmzm_ctx == NULL || !dpmzm_ctx->initialized) {
        return;
    }

    app_dpmzm_handle_command("lock stop");
    if (app_dpmzm_pilot_output_active()) {
        app_dpmzm_handle_command("set pilot-open off");
    }
}

static void switch_runtime_mode(app_runtime_mode_t new_mode)
{
    if (s_runtime_mode == new_mode) {
        return;
    }

    if (new_mode == APP_RUNTIME_MODE_MZM) {
        stop_dpmzm_runtime();
    } else if (new_mode == APP_RUNTIME_MODE_DPMZM) {
        stop_mzm_runtime();
        if (!app_dpmzm_get_context()->initialized) {
            app_dpmzm_init();
        }
    } else {
        stop_mzm_runtime();
        stop_dpmzm_runtime();
    }

    s_runtime_mode = new_mode;
    printf("[app] mode -> %s\r\n", runtime_mode_name(s_runtime_mode));
}

static bool handle_mode_command(const char *cmd)
{
    if (strcmp(cmd, "mode") == 0 || strcmp(cmd, "mode status") == 0) {
        printf("[app] mode: %s\r\n", runtime_mode_name(s_runtime_mode));
        printf("[app] mzm state: %s\r\n", app_state_name(ctx.state));
        if (app_dpmzm_get_context()->initialized) {
            const app_dpmzm_context_t *dpmzm_ctx = app_dpmzm_get_context();
            printf("[app] dpmzm initialized: yes, I=%+.3fV Q=%+.3fV P=%+.3fV\r\n",
                   (double)dpmzm_ctx->bias_i_v,
                   (double)dpmzm_ctx->bias_q_v,
                   (double)dpmzm_ctx->bias_p_v);
        } else {
            printf("[app] dpmzm initialized: no\r\n");
        }
        return true;
    }

    if (strcmp(cmd, "mode mzm") == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_MZM);
        return true;
    }
    if (strcmp(cmd, "mode dpmzm") == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_DPMZM);
        return true;
    }
    if (strcmp(cmd, "mode idle") == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_IDLE);
        return true;
    }

    if (strncmp(cmd, "mode ", 5) == 0) {
        printf("[app] usage: mode idle|mzm|dpmzm|status\r\n");
        return true;
    }

    return false;
}

/* ========================================================================= */
/*  ADC DRDY callback — feeds samples into bias controller                   */
/* ========================================================================= */

static void adc_drdy_callback(const ads131m02_sample_t *sample)
{
    if (!sample->valid) {
        return;
    }

    /* CH0 carries AC pilot response; CH1 carries DC reference. */
    float ac_voltage = ads131m02_code_to_voltage(sample->ch0, ADS131M02_GAIN_1);
    float dc_voltage = ads131m02_code_to_voltage(sample->ch1, ADS131M02_GAIN_1);

    /* Feed into bias controller */
    bool ctrl_updated = bias_ctrl_feed_sample(&ctx.bias_ctrl, ac_voltage, dc_voltage);

    /* If control loop ran, update DAC output */
    if (ctrl_updated || ctx.bias_ctrl.running) {
        float dac_voltage = bias_ctrl_get_dac_output(&ctx.bias_ctrl);
        dac8568_set_voltage(ctx.config->bias_dac_channel, dac_voltage);
    }
}

/* ========================================================================= */
/*  Bias calibration helpers                                                 */
/* ========================================================================= */

static float signed_harmonic(float mag, float phase)
{
    return mag * cosf(phase);
}

static bool measure_harmonics_at_bias(float bias_v, int n_blocks, harmonic_data_t *out)
{
    pilot_gen_t pilot;
    goertzel_state_t g_h1, g_h2;

    if (out == NULL || n_blocks <= 0) {
        return false;
    }

    pilot_gen_init(&pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, ctx.config->pilot_amplitude_v);
    goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    goertzel_init(&g_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

    float h1_i_blocks[CAL_VERIFY_BLOCKS];
    float h1_q_blocks[CAL_VERIFY_BLOCKS];
    float h2_i_blocks[CAL_VERIFY_BLOCKS];
    float h2_q_blocks[CAL_VERIFY_BLOCKS];
    float dc_sum = 0.0f;
    uint32_t dc_count = 0;

    /*
     * Verification jumps between widely separated anchors (null -> peak ->
     * quad), so the small 5 ms settle used earlier was capturing the analog
     * path while it was still moving.  Use the same large-step settle policy
     * as the scan start, then discard one full pilot block before measuring.
     */
    dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
    board_delay_ms(CAL_VERIFY_SETTLE_MS);

    pilot_gen_reset(&pilot);
    for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
        float dac_v = bias_v + pilot_gen_next(&pilot);
        dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

        uint32_t t0 = HAL_GetTick();
        while (board_adc_drdy_read() != 0) {
            if ((HAL_GetTick() - t0) > 5) {
                dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
                return false;
            }
        }

        ads131m02_sample_t smp;
        (void)ads131m02_read_sample(&smp);
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
    board_delay_ms(2);

    for (int b = 0; b < n_blocks; b++) {
        goertzel_reset(&g_h1);
        goertzel_reset(&g_h2);
        pilot_gen_reset(&pilot);

        for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
            float dac_v = bias_v + pilot_gen_next(&pilot);
            dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 5) {
                    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
                    return false;
                }
            }

            ads131m02_sample_t smp;
            if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                goertzel_process_sample(&g_h1, ch0);
                goertzel_process_sample(&g_h2, ch0);
                dc_sum += ch1;
                dc_count++;
            }
        }

        float h1_mag, h1_phase, h2_mag, h2_phase;
        goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
        goertzel_get_result(&g_h2, &h2_mag, &h2_phase);
        h1_i_blocks[b] = h1_mag * cosf(h1_phase);
        h1_q_blocks[b] = h1_mag * sinf(h1_phase);
        h2_i_blocks[b] = h2_mag * cosf(h2_phase);
        h2_q_blocks[b] = h2_mag * sinf(h2_phase);
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

    {
        float h1_i = robust_meanf_local(h1_i_blocks, n_blocks);
        float h1_q = robust_meanf_local(h1_q_blocks, n_blocks);
        float h2_i = robust_meanf_local(h2_i_blocks, n_blocks);
        float h2_q = robust_meanf_local(h2_q_blocks, n_blocks);
        out->h1_magnitude = sqrtf(h1_i * h1_i + h1_q * h1_q);
        out->h1_phase = atan2f(h1_q, h1_i);
        out->h2_magnitude = sqrtf(h2_i * h2_i + h2_q * h2_q);
        out->h2_phase = atan2f(h2_q, h2_i);
        out->dc_power = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;
    }

    return true;
}

static float interp_linear(const float *x, const float *y, int n, float target)
{
    if (n <= 0) {
        return 0.0f;
    }
    if (target <= x[0]) {
        return y[0];
    }
    if (target >= x[n - 1]) {
        return y[n - 1];
    }

    for (int i = 0; i < n - 1; i++) {
        if (target >= x[i] && target <= x[i + 1]) {
            float dx = x[i + 1] - x[i];
            if (fabsf(dx) < 1e-9f) {
                return y[i];
            }
            float t = (target - x[i]) / dx;
            return y[i] + t * (y[i + 1] - y[i]);
        }
    }

    return y[n - 1];
}

static bool solve_3x3(float a[3][4])
{
    for (int i = 0; i < 3; i++) {
        int pivot = i;
        float pivot_abs = fabsf(a[i][i]);
        for (int r = i + 1; r < 3; r++) {
            float cand = fabsf(a[r][i]);
            if (cand > pivot_abs) {
                pivot = r;
                pivot_abs = cand;
            }
        }
        if (pivot_abs < 1e-9f) {
            return false;
        }
        if (pivot != i) {
            for (int c = i; c < 4; c++) {
                float tmp = a[i][c];
                a[i][c] = a[pivot][c];
                a[pivot][c] = tmp;
            }
        }

        float inv = 1.0f / a[i][i];
        for (int c = i; c < 4; c++) {
            a[i][c] *= inv;
        }

        for (int r = 0; r < 3; r++) {
            if (r == i) {
                continue;
            }
            float f = a[r][i];
            for (int c = i; c < 4; c++) {
                a[r][c] -= f * a[i][c];
            }
        }
    }
    return true;
}

static bool fit_three_term_model(const float *b0, const float *b1, const float *y,
                                 int n,
                                 float *c0, float *c1, float *c2,
                                 float *rmse_out)
{
    float s00 = 0.0f, s01 = 0.0f, s02 = 0.0f;
    float s11 = 0.0f, s12 = 0.0f, s22 = 0.0f;
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f;
    float aug[3][4];

    if (n <= 0) {
        return false;
    }

    for (int i = 0; i < n; i++) {
        s00 += b0[i] * b0[i];
        s01 += b0[i] * b1[i];
        s02 += b0[i];
        s11 += b1[i] * b1[i];
        s12 += b1[i];
        s22 += 1.0f;
        t0 += b0[i] * y[i];
        t1 += b1[i] * y[i];
        t2 += y[i];
    }

    aug[0][0] = s00; aug[0][1] = s01; aug[0][2] = s02; aug[0][3] = t0;
    aug[1][0] = s01; aug[1][1] = s11; aug[1][2] = s12; aug[1][3] = t1;
    aug[2][0] = s02; aug[2][1] = s12; aug[2][2] = s22; aug[2][3] = t2;

    if (!solve_3x3(aug)) {
        return false;
    }

    *c0 = aug[0][3];
    *c1 = aug[1][3];
    *c2 = aug[2][3];

    if (rmse_out != NULL) {
        float mse = 0.0f;
        for (int i = 0; i < n; i++) {
            float y_fit = (*c0) * b0[i] + (*c1) * b1[i] + (*c2);
            float err = y[i] - y_fit;
            mse += err * err;
        }
        *rmse_out = sqrtf(mse / (float)n);
    }

    return true;
}

static void smooth_signal_gaussian(const float *x, const float *y, int n,
                                   float sigma_v, float *out)
{
    const float dx_uniform = (n >= 2) ? fabsf(x[1] - x[0]) : 0.0f;

    if (sigma_v <= 1e-6f) {
        for (int i = 0; i < n; i++) {
            out[i] = y[i];
        }
        return;
    }

    if (dx_uniform > 1e-6f) {
        int radius = (int)ceilf(3.0f * sigma_v / dx_uniform);
        if (radius < 1) {
            radius = 1;
        }
        if (radius > 24) {
            radius = 24;
        }

        float kernel[49];
        for (int k = -radius; k <= radius; k++) {
            float dx = (float)k * dx_uniform;
            kernel[k + radius] = expf(-0.5f * (dx / sigma_v) * (dx / sigma_v));
        }

        for (int i = 0; i < n; i++) {
            float weighted_sum = 0.0f;
            float w_sum = 0.0f;
            int j0 = i - radius;
            int j1 = i + radius;
            if (j0 < 0) {
                j0 = 0;
            }
            if (j1 >= n) {
                j1 = n - 1;
            }
            for (int j = j0; j <= j1; j++) {
                float w = kernel[j - i + radius];
                weighted_sum += w * y[j];
                w_sum += w;
            }
            out[i] = (w_sum > 1e-12f) ? (weighted_sum / w_sum) : y[i];
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        float weighted_sum = 0.0f;
        float w_sum = 0.0f;
        for (int j = 0; j < n; j++) {
            float dx = x[i] - x[j];
            float w = expf(-0.5f * (dx / sigma_v) * (dx / sigma_v));
            weighted_sum += w * y[j];
            w_sum += w;
        }
        out[i] = (w_sum > 1e-12f) ? (weighted_sum / w_sum) : y[i];
    }
}

typedef struct {
    const char *label;
    const char *line_prefix;
    float start_v;
    float stop_v;
    float step_v;
    int n_blocks_per_step;
    bool print_scan_lines;
} bias_scan_config_t;

static bool collect_bias_scan(const bias_scan_config_t *cfg, int *steps_done_out)
{
    pilot_gen_t scan_pilot;
    goertzel_state_t g_h1, g_h2;
    int n_steps;
    int steps_done = 0;
    bool drdy_abort = false;

    if (cfg == NULL || steps_done_out == NULL) {
        return false;
    }

    pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                   (float)DSP_SAMPLE_RATE_HZ, ctx.config->pilot_amplitude_v);
    goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
    goertzel_init(&g_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                  (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

    n_steps = (int)((cfg->stop_v - cfg->start_v) / cfg->step_v) + 1;
    if (n_steps > SCAN_MAX_STEPS) {
        n_steps = SCAN_MAX_STEPS;
    }

    printf("[cal] %s: %+.2fV to %+.2fV, step=%.2fV, %d blocks/step\r\n",
           cfg->label,
           (double)cfg->start_v,
           (double)cfg->stop_v,
           (double)cfg->step_v,
           cfg->n_blocks_per_step);

    dac8568_set_voltage(ctx.config->bias_dac_channel, cfg->start_v);
    board_delay_ms(100);

    for (int si = 0; si < n_steps && !drdy_abort; si++) {
        float bias_v = cfg->start_v + (float)si * cfg->step_v;
        float h1_i_sum = 0.0f;
        float h1_q_sum = 0.0f;
        float h2_i_sum = 0.0f;
        float h2_q_sum = 0.0f;
        float dc_sum = 0.0f;
        uint32_t dc_count = 0;

        if (bias_v > cfg->stop_v + cfg->step_v * 0.5f) {
            break;
        }

        dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
        board_delay_ms(2);

        for (int b = 0; b < cfg->n_blocks_per_step && !drdy_abort; b++) {
            goertzel_reset(&g_h1);
            goertzel_reset(&g_h2);
            pilot_gen_reset(&scan_pilot);

            for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                uint32_t t0 = HAL_GetTick();
                while (board_adc_drdy_read() != 0) {
                    if ((HAL_GetTick() - t0) > 5) {
                        drdy_abort = true;
                        break;
                    }
                }
                if (drdy_abort) {
                    break;
                }

                ads131m02_sample_t smp;
                if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                    float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                    float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                    goertzel_process_sample(&g_h1, ch0);
                    goertzel_process_sample(&g_h2, ch0);
                    dc_sum += ch1;
                    dc_count++;
                }
            }

            if (!drdy_abort) {
                float h1_mag, h1_phase, h2_mag, h2_phase;
                goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                goertzel_get_result(&g_h2, &h2_mag, &h2_phase);
                h1_i_sum += h1_mag * cosf(h1_phase);
                h1_q_sum += h1_mag * sinf(h1_phase);
                h2_i_sum += h2_mag * cosf(h2_phase);
                h2_q_sum += h2_mag * sinf(h2_phase);
            }
        }

        if (drdy_abort) {
            break;
        }

        {
            float inv_blocks = 1.0f / (float)cfg->n_blocks_per_step;
            float h1_i = h1_i_sum * inv_blocks;
            float h1_q = h1_q_sum * inv_blocks;
            float h2_i = h2_i_sum * inv_blocks;
            float h2_q = h2_q_sum * inv_blocks;
            float h1_mag = sqrtf(h1_i * h1_i + h1_q * h1_q);
            float h1_phase = atan2f(h1_q, h1_i);
            float h1_signed = h1_mag * cosf(h1_phase);
            float h2_mag = sqrtf(h2_i * h2_i + h2_q * h2_q);
            float h2_phase = atan2f(h2_q, h2_i);
            /* Use Q-component (sinf) — hardware delay shifts H2 phase ≈90°. */
            float h2_signed = h2_mag * sinf(h2_phase);
            float dc_mean = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;

            s_scan_bias_buf[steps_done] = bias_v;
            s_scan_h1_mag_buf[steps_done] = h1_mag;
            s_scan_h1_signed_buf[steps_done] = h1_signed;
            s_scan_h2_signed_buf[steps_done] = h2_signed;
            s_scan_dc_buf[steps_done] = dc_mean;
            steps_done++;

            if (cfg->print_scan_lines && cfg->line_prefix != NULL) {
                printf("%s %+.3f %.6f %.6f %.6f %.6f %.6f\r\n",
                       cfg->line_prefix,
                       (double)bias_v,
                       (double)h1_mag,
                       (double)h1_signed,
                       (double)h2_mag,
                       (double)h2_signed,
                       (double)dc_mean);
            }
        }
    }

    dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

    if (drdy_abort || steps_done < 4) {
        printf("[cal] %s failed%s\r\n",
               cfg->label,
               drdy_abort ? " (DRDY timeout)" : "");
        return false;
    }

    *steps_done_out = steps_done;
    return true;
}

static bool analyze_bias_scan_anchors(int steps_done, bias_cal_result_t *result)
{
    float h1_max = 0.0f;
    float min_v[16];
    float min_dc[16];
    int n_mins = 0;
    int central_pair = 0;
    float best_mid_abs = FLT_MAX;

    if (result == NULL || steps_done < 4) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    for (int i = 0; i < steps_done; i++) {
        if (s_scan_h1_mag_buf[i] > h1_max) {
            h1_max = s_scan_h1_mag_buf[i];
        }
    }
    if (h1_max < 1e-7f) {
        printf("[cal] no H1 signal detected\r\n");
        return false;
    }

    {
        float min_threshold = h1_max * 0.1f;
        for (int i = 1; i < steps_done - 1 && n_mins < 16; i++) {
            if (s_scan_h1_mag_buf[i] < s_scan_h1_mag_buf[i - 1] &&
                s_scan_h1_mag_buf[i] < s_scan_h1_mag_buf[i + 1] &&
                s_scan_h1_mag_buf[i] < min_threshold) {
                float a = s_scan_h1_mag_buf[i - 1];
                float b = s_scan_h1_mag_buf[i];
                float c = s_scan_h1_mag_buf[i + 1];
                float denom = a - 2.0f * b + c;
                float offset = (fabsf(denom) > 1e-12f) ? (0.5f * (a - c) / denom) : 0.0f;
                if (offset < -1.0f) offset = -1.0f;
                if (offset > 1.0f)  offset = 1.0f;
                min_v[n_mins] = s_scan_bias_buf[i] + offset * (s_scan_bias_buf[1] - s_scan_bias_buf[0]);
                min_dc[n_mins] = interp_linear(s_scan_bias_buf, s_scan_dc_buf, steps_done, min_v[n_mins]);
                n_mins++;
            }
        }
    }

    if (n_mins < 2) {
        printf("[cal] insufficient H1 minima found\r\n");
        return false;
    }

    for (int i = 0; i < n_mins - 1; i++) {
        result->vpi_v += min_v[i + 1] - min_v[i];
        {
            float mid = 0.5f * (min_v[i] + min_v[i + 1]);
            if (fabsf(mid) < best_mid_abs) {
                best_mid_abs = fabsf(mid);
                central_pair = i;
            }
        }
    }
    result->vpi_v /= (float)(n_mins - 1);

    if (min_dc[central_pair] < min_dc[central_pair + 1]) {
        result->null_v = min_v[central_pair];
        result->peak_v = min_v[central_pair + 1];
    } else {
        result->peak_v = min_v[central_pair];
        result->null_v = min_v[central_pair + 1];
    }

    if (result->peak_v <= result->null_v) {
        printf("[cal] canonical period ordering invalid: null=%+.3fV peak=%+.3fV\r\n",
               (double)result->null_v, (double)result->peak_v);
        return false;
    }

    {
        bool quad_pos_valid = false;
        bool quad_neg_valid = false;
        for (int i = 0; i < n_mins - 1; i++) {
            float mid = 0.5f * (min_v[i] + min_v[i + 1]);
            bool rising = (min_dc[i + 1] > min_dc[i]);
            if (rising) {
                if (!quad_pos_valid || fabsf(mid) < fabsf(result->quad_pos_v)) {
                    result->quad_pos_v = mid;
                    quad_pos_valid = true;
                }
            } else {
                if (!quad_neg_valid || fabsf(mid) < fabsf(result->quad_neg_v)) {
                    result->quad_neg_v = mid;
                    quad_neg_valid = true;
                }
            }
        }

        if (!quad_pos_valid) {
            result->quad_pos_v = 0.5f * (result->null_v + result->peak_v);
        }
        if (!quad_neg_valid) {
            result->quad_neg_v = result->quad_pos_v - result->vpi_v;
        }
    }

    result->valid = true;
    return true;
}

static void fit_affine_from_current_scan(int steps_done, bias_cal_result_t *result)
{
    if (result == NULL) {
        return;
    }

    result->affine_valid = false;
    result->affine_o1 = 0.0f;
    result->affine_o2 = 0.0f;
    result->affine_m11 = 0.0f;
    result->affine_m12 = 0.0f;
    result->affine_m21 = 0.0f;
    result->affine_m22 = 0.0f;

    if (result->vpi_v <= 1e-6f) {
        return;
    }

    {
        float omega = (float)M_PI / result->vpi_v;
        float h1_csin = 0.0f;
        float h1_ccos = 0.0f;
        float h1_off = 0.0f;
        float h1_rmse = 0.0f;

        for (int i = 0; i < steps_done; i++) {
            s_fit_sin_basis[i] = sinf(omega * s_scan_bias_buf[i]);
            s_fit_cos_basis[i] = cosf(omega * s_scan_bias_buf[i]);
        }

        if (fit_three_term_model(s_fit_sin_basis, s_fit_cos_basis, s_scan_h1_signed_buf, steps_done,
                                 &h1_csin, &h1_ccos, &h1_off, &h1_rmse)) {
            float psi = atan2f(h1_ccos, h1_csin);
            float h1_amp = sqrtf(h1_csin * h1_csin + h1_ccos * h1_ccos);
            float h2_csin = 0.0f;
            float h2_ccos = 0.0f;
            float h2_off = 0.0f;
            float h2_rmse = 0.0f;

            for (int i = 0; i < steps_done; i++) {
                s_fit_phi_basis[i] = omega * s_scan_bias_buf[i] + psi;
                s_fit_sin_phi_basis[i] = sinf(s_fit_phi_basis[i]);
                s_fit_cos_phi_basis[i] = cosf(s_fit_phi_basis[i]);
            }
            smooth_signal_gaussian(s_scan_bias_buf, s_scan_h2_signed_buf, steps_done,
                                   0.25f, s_fit_h2_smooth_buf);

            if (fit_three_term_model(s_fit_sin_phi_basis, s_fit_cos_phi_basis, s_scan_h1_signed_buf,
                                     steps_done,
                                     &result->affine_m11, &result->affine_m12,
                                     &result->affine_o1, NULL) &&
                fit_three_term_model(s_fit_sin_phi_basis, s_fit_cos_phi_basis, s_fit_h2_smooth_buf,
                                     steps_done,
                                     &h2_csin, &h2_ccos, &h2_off, &h2_rmse)) {
                float det;

                result->affine_m21 = h2_csin;
                result->affine_m22 = h2_ccos;
                result->affine_o2 = h2_off;
                det = result->affine_m11 * result->affine_m22
                    - result->affine_m12 * result->affine_m21;

                if (fabsf(det) > 1e-8f && h1_amp > 1e-4f) {
                    result->affine_valid = true;
                    printf("[cal] affine: o=(%+.5f,%+.5f)  "
                           "M=[[%.5f, %.5f],[%.6f, %.6f]]  "
                           "rmse=(%.5fmV, %.5fmV)\r\n",
                           (double)result->affine_o1,
                           (double)result->affine_o2,
                           (double)result->affine_m11,
                           (double)result->affine_m12,
                           (double)result->affine_m21,
                           (double)result->affine_m22,
                           (double)(h1_rmse * 1000.0f),
                           (double)(h2_rmse * 1000.0f));
                }
            }
        }
    }
}

static bool build_harmonic_axis_model(bias_cal_result_t *result)
{
    harmonic_data_t h_null = {0}, h_peak = {0}, h_quad_pos = {0}, h_quad_neg = {0};
    bool ok_null;
    bool ok_peak;
    bool ok_qp;
    bool ok_qn;
    float h1s_null;
    float h2s_null;
    float h1s_peak;
    float h2s_peak;
    float h1s_qp;
    float h2s_qp;
    float h1s_qn;
    float h2s_qn;
    float off1_sum = 0.0f;
    float a1_sum = 0.0f;
    float a2_sum = 0.0f;
    int off1_count = 0;
    int a1_count = 0;
    int a2_count = 0;

    if (result == NULL) {
        return false;
    }

    ok_null = measure_harmonics_at_bias(result->null_v, CAL_VERIFY_BLOCKS, &h_null);
    ok_peak = measure_harmonics_at_bias(result->peak_v, CAL_VERIFY_BLOCKS, &h_peak);
    ok_qp = measure_harmonics_at_bias(result->quad_pos_v, CAL_VERIFY_BLOCKS, &h_quad_pos);
    ok_qn = measure_harmonics_at_bias(result->quad_neg_v, CAL_VERIFY_BLOCKS, &h_quad_neg);

    if (ok_null && ok_peak && h_peak.dc_power < h_null.dc_power) {
        float tmp = result->null_v;
        result->null_v = result->peak_v;
        result->peak_v = tmp;
        {
            harmonic_data_t htmp = h_null;
            h_null = h_peak;
            h_peak = htmp;
        }
    }

    if (result->peak_v <= result->null_v) {
        printf("[cal] refined period ordering invalid: null=%+.3fV peak=%+.3fV\r\n",
               (double)result->null_v, (double)result->peak_v);
        return false;
    }

    /* DC calibration: TIA output at extinction and peak transmission */
    result->dc_cal_valid = ok_null && ok_peak;
    result->dc_null_v = ok_null ? h_null.dc_power : 0.0f;
    result->dc_peak_v = ok_peak ? h_peak.dc_power : 0.0f;
    if (result->dc_cal_valid) {
        printf("[cal] DC cal: null=%.4fV  peak=%.4fV  range=%.4fV\r\n",
               (double)result->dc_null_v,
               (double)result->dc_peak_v,
               (double)(result->dc_peak_v - result->dc_null_v));
    }

    if (ok_qp && ok_qn) {
        float h1s_qp_cmp = signed_harmonic(h_quad_pos.h1_magnitude, h_quad_pos.h1_phase);
        float h1s_qn_cmp = signed_harmonic(h_quad_neg.h1_magnitude, h_quad_neg.h1_phase);
        if (h1s_qp_cmp < h1s_qn_cmp) {
            float tmp = result->quad_pos_v;
            result->quad_pos_v = result->quad_neg_v;
            result->quad_neg_v = tmp;
            {
                harmonic_data_t htmp2 = h_quad_pos;
                h_quad_pos = h_quad_neg;
                h_quad_neg = htmp2;
            }
        }
    }

    h1s_null = ok_null ? signed_harmonic(h_null.h1_magnitude, h_null.h1_phase) : 0.0f;
    /* Use Q-component (sinf) for H2 to match observer convention. */
    h2s_null = ok_null ? h_null.h2_magnitude * sinf(h_null.h2_phase) : 0.0f;
    h1s_peak = ok_peak ? signed_harmonic(h_peak.h1_magnitude, h_peak.h1_phase) : 0.0f;
    h2s_peak = ok_peak ? h_peak.h2_magnitude * sinf(h_peak.h2_phase) : 0.0f;
    h1s_qp = ok_qp ? signed_harmonic(h_quad_pos.h1_magnitude, h_quad_pos.h1_phase) : 0.0f;
    h2s_qp = ok_qp ? h_quad_pos.h2_magnitude * sinf(h_quad_pos.h2_phase) : 0.0f;
    h1s_qn = ok_qn ? signed_harmonic(h_quad_neg.h1_magnitude, h_quad_neg.h1_phase) : 0.0f;
    h2s_qn = ok_qn ? h_quad_neg.h2_magnitude * sinf(h_quad_neg.h2_phase) : 0.0f;

    if (ok_null) {
        printf("[cal] null test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
               (double)result->null_v,
               (double)h1s_null,
               (double)h2s_null,
               (double)h_null.dc_power);
    }
    if (ok_peak) {
        printf("[cal] peak test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
               (double)result->peak_v,
               (double)h1s_peak,
               (double)h2s_peak,
               (double)h_peak.dc_power);
    }
    if (ok_qp) {
        printf("[cal] quad+ test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
               (double)result->quad_pos_v,
               (double)h1s_qp,
               (double)h2s_qp,
               (double)h_quad_pos.dc_power);
    }
    if (ok_qn) {
        printf("[cal] quad- test: V=%+.3f H1s=%+.5f H2s=%+.5f DC=%.5f\r\n",
               (double)result->quad_neg_v,
               (double)h1s_qn,
               (double)h2s_qn,
               (double)h_quad_neg.dc_power);
    }

    result->harmonics_valid = false;
    result->h1_offset = 0.0f;
    result->h2_offset = 0.0f;
    result->h1_axis = 0.0f;
    result->h2_axis = 0.0f;
    result->h1_axis_sign = 1.0f;
    result->h2_axis_sign = 1.0f;
    result->pilot_cal_v = ctx.config->pilot_amplitude_v;

    if (ok_null) {
        off1_sum += h1s_null;
        off1_count++;
    }
    if (ok_peak) {
        off1_sum += h1s_peak;
        off1_count++;
    }
    if (off1_count > 0) {
        result->h1_offset = off1_sum / (float)off1_count;
    }

    if (ok_qp) {
        result->h2_offset = h2s_qp;
    } else if (ok_qn) {
        result->h2_offset = h2s_qn;
    }

    if (ok_qp) {
        a1_sum += fabsf(h1s_qp - result->h1_offset);
        a1_count++;
        result->h1_axis_sign = ((h1s_qp - result->h1_offset) >= 0.0f) ? 1.0f : -1.0f;
    }
    if (ok_qn) {
        a1_sum += fabsf(h1s_qn - result->h1_offset);
        a1_count++;
        if (!ok_qp) {
            result->h1_axis_sign = ((h1s_qn - result->h1_offset) <= 0.0f) ? 1.0f : -1.0f;
        }
    }

    if (ok_null) {
        a2_sum += fabsf(h2s_null);
        a2_count++;
        result->h2_axis_sign = (h2s_null >= 0.0f) ? 1.0f : -1.0f;
    }
    if (ok_peak) {
        a2_sum += fabsf(h2s_peak);
        a2_count++;
        if (!ok_null) {
            result->h2_axis_sign = (h2s_peak <= 0.0f) ? 1.0f : -1.0f;
        } else {
            result->h2_axis_sign = ((h2s_null - h2s_peak) >= 0.0f) ? 1.0f : -1.0f;
        }
    }

    if (a1_count > 0) {
        result->h1_axis = a1_sum / (float)a1_count;
    }
    if (a2_count > 0) {
        result->h2_axis = a2_sum / (float)a2_count;
    }

    if (result->h1_axis > 1e-4f && result->h2_axis > 1e-5f &&
        fabsf(result->h1_axis_sign) > 0.5f &&
        fabsf(result->h2_axis_sign) > 0.5f) {
        result->harmonics_valid = true;
        printf("[cal] axis cal: off1=%+.5f off2=%+.5f  "
               "A1=%.5f sign1=%+.0f  A2=%.5f sign2=%+.0f  pilot=%.1fmV\r\n",
               (double)result->h1_offset,
               (double)result->h2_offset,
               (double)result->h1_axis,
               (double)result->h1_axis_sign,
               (double)result->h2_axis,
               (double)result->h2_axis_sign,
               (double)(result->pilot_cal_v * 1000.0f));
    }

    /*
     * obs_dc seed is disabled: the single-shot H2 measurement at QUAD is
     * noise-dominated (H2 → 0 at QUAD), producing an unreliable obs_y estimate
     * that over-corrects and prevents locking.  The EMA in the control loop
     * converges to the true b_obs naturally after lock, then the spring corrects
     * the bias to true QUAD.  No seed needed.
     */
    result->obs_y_quad = 0.0f;
    result->obs_y_quad_valid = false;

    return result->harmonics_valid;
}

static bool run_bias_calibration_scan(bias_cal_result_t *result, bool print_scan_lines)
{
    bias_cal_result_t coarse = {0};
    bias_scan_config_t pass1_cfg;
    bias_scan_config_t pass2_cfg;
    int steps_done = 0;
    float zero_left_v;
    float zero_right_v;

    if (result == NULL) {
        return false;
    }

    pass1_cfg.label = "pass1 fast scan";
    pass1_cfg.line_prefix = "CALSCAN1";
    pass1_cfg.start_v = -(10.0f - ctx.config->pilot_amplitude_v);
    pass1_cfg.stop_v = (10.0f - ctx.config->pilot_amplitude_v);
    pass1_cfg.step_v = 0.1f;
    pass1_cfg.n_blocks_per_step = 3;
    pass1_cfg.print_scan_lines = print_scan_lines;

    if (!collect_bias_scan(&pass1_cfg, &steps_done) ||
        !analyze_bias_scan_anchors(steps_done, &coarse)) {
        return false;
    }

    zero_left_v = fminf(coarse.null_v, coarse.peak_v);
    zero_right_v = fmaxf(coarse.null_v, coarse.peak_v);
    printf("[cal] pass1 canonical period: left=%+.3fV right=%+.3fV Vpi=%.3fV\r\n",
           (double)zero_left_v,
           (double)zero_right_v,
           (double)coarse.vpi_v);

    pass2_cfg.label = "pass2 local scan";
    pass2_cfg.line_prefix = "CALSCAN2";
    pass2_cfg.start_v = zero_left_v - 0.15f * coarse.vpi_v;
    pass2_cfg.stop_v = zero_right_v + 0.15f * coarse.vpi_v;
    pass2_cfg.step_v = 0.05f;
    pass2_cfg.n_blocks_per_step = 10;
    pass2_cfg.print_scan_lines = print_scan_lines;

    if (!collect_bias_scan(&pass2_cfg, &steps_done) ||
        !analyze_bias_scan_anchors(steps_done, result)) {
        return false;
    }

    fit_affine_from_current_scan(steps_done, result);
    if (!build_harmonic_axis_model(result)) {
        printf("[cal] harmonic-axis calibration failed\r\n");
        return false;
    }

    result->valid = true;
    return true;
}

static void commit_bias_calibration(const bias_cal_result_t *result)
{
    ctx.config->bias_cal_valid = result->valid;
    ctx.config->vpi_v = result->vpi_v;
    ctx.config->bias_null_v = result->null_v;
    ctx.config->bias_peak_v = result->peak_v;
    ctx.config->bias_quad_pos_v = result->quad_pos_v;
    ctx.config->bias_quad_neg_v = result->quad_neg_v;
    ctx.config->cal_dc_valid  = result->dc_cal_valid;
    ctx.config->cal_dc_null_v = result->dc_null_v;
    ctx.config->cal_dc_peak_v = result->dc_peak_v;
    mzm_set_calibration(result->valid,
                        result->vpi_v,
                        result->null_v,
                        result->peak_v,
                        result->quad_pos_v,
                        result->quad_neg_v,
                        result->dc_null_v,
                        result->dc_peak_v);

    /* Store harmonic-axis calibration and push to the MZM strategy */
    ctx.config->cal_harmonics_valid = result->harmonics_valid;
    ctx.config->cal_h1_offset = result->h1_offset;
    ctx.config->cal_h2_offset = result->h2_offset;
    ctx.config->cal_h1_axis = result->h1_axis;
    ctx.config->cal_h2_axis = result->h2_axis;
    ctx.config->cal_h1_axis_sign = result->h1_axis_sign;
    ctx.config->cal_h2_axis_sign = result->h2_axis_sign;
    ctx.config->cal_pilot_amplitude_v = result->pilot_cal_v;
    mzm_set_harmonic_axes(result->harmonics_valid,
                          result->h1_offset,
                          result->h2_offset,
                          result->h1_axis,
                          result->h2_axis,
                          result->h1_axis_sign,
                          result->h2_axis_sign,
                          result->pilot_cal_v);
    ctx.config->cal_affine_valid = result->affine_valid;
    ctx.config->cal_affine_o1 = result->affine_o1;
    ctx.config->cal_affine_o2 = result->affine_o2;
    ctx.config->cal_affine_m11 = result->affine_m11;
    ctx.config->cal_affine_m12 = result->affine_m12;
    ctx.config->cal_affine_m21 = result->affine_m21;
    ctx.config->cal_affine_m22 = result->affine_m22;
    mzm_set_affine_model(result->affine_valid,
                         result->affine_o1,
                         result->affine_o2,
                         result->affine_m11,
                         result->affine_m12,
                         result->affine_m21,
                         result->affine_m22,
                         result->pilot_cal_v);

    if (result->obs_y_quad_valid) {
        mzm_set_obs_dc_seed(result->obs_y_quad);
    }

    printf("[cal] Vpi=%.3fV  null=%+.3fV  peak=%+.3fV  quad+=%+.3fV  quad-=%+.3fV\r\n",
           (double)ctx.config->vpi_v,
           (double)ctx.config->bias_null_v,
           (double)ctx.config->bias_peak_v,
           (double)ctx.config->bias_quad_pos_v,
           (double)ctx.config->bias_quad_neg_v);
}

static float seed_bias_from_calibration(void)
{
    if (!ctx.config->bias_cal_valid || ctx.config->vpi_v <= 0.0f) {
        return ctx.config->initial_bias_v;
    }

    switch (ctx.config->target_point) {
    case BIAS_POINT_MIN:
        return ctx.config->bias_null_v;
    case BIAS_POINT_MAX:
        return ctx.config->bias_peak_v;
    case BIAS_POINT_QUAD:
        return ctx.config->bias_quad_pos_v;
    case BIAS_POINT_CUSTOM:
        return ctx.config->bias_null_v +
               ctx.config->vpi_v * (ctx.config->target_phase_rad / (float)M_PI);
    default:
        return ctx.config->bias_quad_pos_v;
    }
}

/* ========================================================================= */
/*  State handlers                                                           */
/* ========================================================================= */

static void state_init(void)
{
    /* Initialize board hardware */
    board_init();

    /* Start USART1 DMA receive — must come after MX_USART1_UART_Init() */
    app_uart_init();

    /* Load or set default configuration */
    app_config_load();
    ctx.config = app_config_get();
    mzm_set_calibration(ctx.config->bias_cal_valid,
                        ctx.config->vpi_v,
                        ctx.config->bias_null_v,
                        ctx.config->bias_peak_v,
                        ctx.config->bias_quad_pos_v,
                        ctx.config->bias_quad_neg_v,
                        ctx.config->cal_dc_null_v,
                        ctx.config->cal_dc_peak_v);

    /* Restore harmonic-axis calibration into the MZM strategy (survives warm reset) */
    mzm_set_harmonic_axes(ctx.config->cal_harmonics_valid,
                          ctx.config->cal_h1_offset,
                          ctx.config->cal_h2_offset,
                          ctx.config->cal_h1_axis,
                          ctx.config->cal_h2_axis,
                          ctx.config->cal_h1_axis_sign,
                          ctx.config->cal_h2_axis_sign,
                          ctx.config->cal_pilot_amplitude_v);
    mzm_set_affine_model(ctx.config->cal_affine_valid,
                         ctx.config->cal_affine_o1,
                         ctx.config->cal_affine_o2,
                         ctx.config->cal_affine_m11,
                         ctx.config->cal_affine_m12,
                         ctx.config->cal_affine_m21,
                         ctx.config->cal_affine_m22,
                         ctx.config->cal_pilot_amplitude_v);

    transition_to(APP_STATE_HW_SELFTEST);
}

static void state_selftest(void)
{
    /* Initialize DAC8568 — non-fatal if chip is not yet soldered */
    int ret = dac8568_init();
    if (ret != 0) {
        printf("[app] WARNING: DAC8568 init failed (%d), continuing without DAC\r\n", ret);
    }

    /* Initialize ADS131M02 — fatal if ADC fails */
    ret = ads131m02_init();
    if (ret != 0) {
        printf("[app] FAULT: ADS131M02 init failed (%d)\r\n", ret);
        transition_to(APP_STATE_FAULT);
        return;
    }

    /* Enable DAC DMA (spec-07).  Smoke test must pass — DMA failure
     * means the SPI1 TxCplt chain is unreliable and the system enters FAULT. */
    if (dac8568_enable_dma() != 0) {
        printf("[app] FAULT: DAC DMA smoke test failed\r\n");
        transition_to(APP_STATE_FAULT);
        return;
    }

    /* TODO: Selftest — write known DAC value, read back via ADC, verify */

    /* Blink LED to indicate successful init */
    board_led_on();
    board_delay_ms(200);
    board_led_off();

    transition_to(APP_STATE_IDLE);
}

static void state_idle(void)
{
    /* Waiting for user command (via UART) to start bias control.
     * LED slow blink to indicate idle. */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 500) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
}

static void state_sweeping(void)
{
    if (!ctx.config->bias_cal_valid || !ctx.config->cal_harmonics_valid) {
        bias_cal_result_t result = {0};
        if (!run_bias_calibration_scan(&result, false)) {
            transition_to(APP_STATE_IDLE);
            return;
        }
        commit_bias_calibration(&result);
        if (!result.harmonics_valid) {
            printf("[cal] harmonic-axis calibration failed\r\n");
            transition_to(APP_STATE_IDLE);
            return;
        }
    }

    /* Seed the controller near the calibrated working-point anchor. */
    ctx.bias_ctrl.bias_voltage = seed_bias_from_calibration();

    /*
     * Narrow the PI output window to ±Vπ/2 around the calibrated target so
     * the integrator cannot drift to an adjacent MZM period.  Applied here
     * (after calibration) rather than in the "start" command handler because
     * the calibration scan may run inside state_sweeping(), populating the
     * cal values that are needed to compute the limits.
     * MIN/MAX keep the full ±10 V range.
     */
    if ((ctx.bias_ctrl.target_point == BIAS_POINT_QUAD ||
         ctx.bias_ctrl.target_point == BIAS_POINT_CUSTOM) &&
        ctx.config->bias_cal_valid && ctx.config->vpi_v > 0.0f) {
        float target_v = seed_bias_from_calibration();
        /*
         * Use 0.4×Vπ (not 0.5×Vπ) so the limits stop at φ = ±0.4π from
         * QUAD, not at null/peak.  This keeps the controller on the local
         * branch while still leaving enough room for acquisition transients.
         */
        float half_vpi = ctx.config->vpi_v * 0.4f;
        float lo = target_v - half_vpi;
        float hi = target_v + half_vpi;
        if (lo < -10.0f) lo = -10.0f;
        if (hi >  10.0f) hi =  10.0f;
        bias_ctrl_set_output_limits(&ctx.bias_ctrl, lo, hi);
        printf("[ctrl] QUAD output window: [%.3fV, %.3fV]\r\n", (double)lo, (double)hi);
    }

    /* Start closed-loop control */
    bias_ctrl_start(&ctx.bias_ctrl);
    ads131m02_start_continuous(adc_drdy_callback);

    transition_to(APP_STATE_LOCKING);
}

static void state_locking(void)
{
    /* LED fast blink during locking */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 100) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }

    /* Check if locked */
    if (bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        transition_to(APP_STATE_LOCKED);
        return;
    }

    /* Timeout — lock failed */
    if (elapsed > ctx.config->lock_timeout_ms) {
        /* Retry sweep */
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_SWEEPING);
    }
}

static void state_locked(void)
{
    /* LED solid on when locked */
    board_led_on();

    /* Monitor lock quality */
    if (!bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        /* Lock lost — go back to locking */
        transition_to(APP_STATE_LOCKING);
    }
}

static void state_fault(void)
{
    /* LED rapid blink for fault indication */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 50) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
    /* DAC safe-state write intentionally omitted here —
     * dac8568_write_channel() in a tight fault loop would block
     * if the DMA link is not established. */
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void app_init(void)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = APP_STATE_INIT;
    ctx.tick_ms = 0;
}

void app_run(void)
{
    /* Update tick from HAL systick */
    ctx.tick_ms = HAL_GetTick();

    /* Dispatch any pending UART command (must run in main-loop context, not ISR) */
    app_uart_process();

    /* DAC DMA watchdog (spec-07): any timeout means the TxCplt chain broke. */
    if (dac8568_dma_timeout_count() > 0) {
        printf("[app] FAULT: DAC DMA timeout (%lu)\r\n",
               dac8568_dma_timeout_count());
        transition_to(APP_STATE_FAULT);
        return;
    }

    if (ctx.state == APP_STATE_IDLE) {
        if (s_runtime_mode == APP_RUNTIME_MODE_DPMZM) {
            app_dpmzm_run();
            return;
        }
        if (s_runtime_mode == APP_RUNTIME_MODE_IDLE) {
            return;
        }
    }

    switch (ctx.state) {
    case APP_STATE_INIT:
        state_init();
        break;
    case APP_STATE_HW_SELFTEST:
        state_selftest();
        break;
    case APP_STATE_IDLE:
        state_idle();
        break;
    case APP_STATE_SWEEPING:
        state_sweeping();
        break;
    case APP_STATE_LOCKING:
        state_locking();
        break;
    case APP_STATE_LOCKED:
        state_locked();
        break;
    case APP_STATE_FAULT:
        state_fault();
        break;
    default:
        transition_to(APP_STATE_FAULT);
        break;
    }
}

const app_context_t *app_get_context(void)
{
    return &ctx;
}

void app_handle_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }

    if (handle_mode_command(cmd)) {
        return;
    }

    if (strcmp(cmd, "dpmzm") == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_DPMZM);
        app_dpmzm_handle_command("status");
        return;
    }
    if (strncmp(cmd, "dpmzm ", 6) == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_DPMZM);
        app_dpmzm_handle_command(cmd + 6);
        return;
    }

    if (strcmp(cmd, "mzm") == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_MZM);
        cmd = "status";
    } else if (strncmp(cmd, "mzm ", 4) == 0) {
        switch_runtime_mode(APP_RUNTIME_MODE_MZM);
        cmd += 4;
    } else if (s_runtime_mode == APP_RUNTIME_MODE_DPMZM) {
        app_dpmzm_handle_command(cmd);
        return;
    } else if (s_runtime_mode == APP_RUNTIME_MODE_IDLE) {
        printf("[app] mode is idle; use 'mode mzm' or 'mode dpmzm'\r\n");
        return;
    }

    if (strcmp(cmd, "start") == 0) {
        if (ctx.state == APP_STATE_IDLE) {
            /* Initialize bias controller with current config */
            mzm_set_custom_phase(ctx.config->target_phase_rad);
            mzm_set_pilot_amplitude(ctx.config->pilot_amplitude_v);
            /*
             * Near QUAD the error signal is H2 ≈ cos(φ) → 0 and is very
             * noisy (H2 ∝ J2(m), small for our pilot depth).  The observer
             * (IQ-EMA + unit-vector EMA) has an effective bandwidth far below
             * the control-loop update rate.  With ki=10 the PI moves the bias
             * ~0.5 V/update, faster than the observer can track; the result
             * is that a wrong-sign initial error drives the bias to the ±10 V
             * clamp before the observer self-corrects.  Reducing ki to 2.5
             * limits the per-update step to ~0.05 V, which the observer can
             * follow, while the proportional term still provides fast damping.
             * MIN and MAX do not need this: H1 is large and well-defined
             * there, so the observer converges quickly at any ki.
             */
            /*
             * Near QUAD (and CUSTOM) the error signal is H2 ≈ cos(φ) → 0
             * with significant noise (H2 ∝ J2(m), tiny for our pilot depth).
             *
             * Two pathologies arise with the default kp=1 / ki=10 gains:
             *   1. kp=1 immediately amplifies noise into ≥0.3 V voltage steps,
             *      driving the bias well outside one Vpi period per update.
             *   2. ki=10 winds the integrator up to ±10 V (int_max = out_max /
             *      ki is now enforced in pid_init, but ki=10 still gives a
             *      0.5 V/update increment at error=0.5, faster than the
             *      observer can track).
             *
             * After moving from 10 Hz to 5 Hz control updates, dt doubles.
             * Keep the effective integral action roughly unchanged by halving
             * ki for each target class.
             *
             * For QUAD/CUSTOM: use a more conservative PI than MIN/MAX.
             * At 5 Hz the no-DC phase map is noticeably more oscillatory during
             * acquisition, so keep both proportional and integral action small
             * enough that the observer can settle before the bias swings across
             * the whole local period.
             * MIN/MAX use H1 which is large and reliable — no change needed.
             */
            float kp_eff = ctx.config->kp;
            float ki_eff = ctx.config->ki;
            if (ctx.config->target_point == BIAS_POINT_QUAD ||
                ctx.config->target_point == BIAS_POINT_CUSTOM) {
                kp_eff = 0.05f;
                ki_eff = 0.75f;
            }
            bias_ctrl_init(&ctx.bias_ctrl,
                           ctx.config->modulator_type,
                           ctx.config->target_point,
                           ctx.config->initial_bias_v,
                           ctx.config->pilot_amplitude_v,
                           kp_eff,
                           ki_eff);
            transition_to(APP_STATE_SWEEPING);
        }
    } else if (strcmp(cmd, "stop") == 0) {
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_IDLE);
    } else if (strcmp(cmd, "status") == 0) {
        const harmonic_data_t *h = bias_ctrl_get_harmonics(&ctx.bias_ctrl);
        float ctrl_error = ctx.bias_ctrl.last_error;
        printf("Mode:  %s\r\n", runtime_mode_name(s_runtime_mode));
        printf("State: %s\r\n", app_state_name(ctx.state));
        printf("Bias:  %.3f V\r\n", (double)bias_ctrl_get_bias_voltage(&ctx.bias_ctrl));
        printf("Lock:  %s\r\n", bias_ctrl_is_locked(&ctx.bias_ctrl) ? "YES" : "NO");
        if (ctx.config->bias_cal_valid) {
            printf("Cal:   Vpi=%.3fV  null=%+.3fV  peak=%+.3fV  quad+=%+.3fV\r\n",
                   (double)ctx.config->vpi_v,
                   (double)ctx.config->bias_null_v,
                   (double)ctx.config->bias_peak_v,
                   (double)ctx.config->bias_quad_pos_v);
        } else {
            printf("Cal:   INVALID\r\n");
        }
        if (ctx.config->cal_dc_valid) {
            printf("DCCal: null=%.4fV  peak=%.4fV\r\n",
                   (double)ctx.config->cal_dc_null_v,
                   (double)ctx.config->cal_dc_peak_v);
        }
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            float dc = h->dc_power > 1e-6f ? h->dc_power : 1e-6f;
            printf("H1:    %.4fV (%.1fdBc vs DC)  phase=%.3frad\r\n",
                   (double)h->h1_magnitude,
                   (double)(20.0f * log10f(h->h1_magnitude / dc + 1e-9f)),
                   (double)h->h1_phase);
            printf("H2:    %.4fV (%.1fdBc vs DC)  phase=%.3frad\r\n",
                   (double)h->h2_magnitude,
                   (double)(20.0f * log10f(h->h2_magnitude / dc + 1e-9f)),
                   (double)h->h2_phase);
            printf("DC:    %.4fV\r\n", (double)h->dc_power);
            printf("Err:   %.4f\r\n", (double)ctrl_error);
            printf("Term:  raw=%+.4f dc=%+.4f obs=%+.4f spring=%+.4f dcoff=%+.4fV\r\n",
                   (double)ctx.bias_ctrl.diag_error_obs_raw,
                   (double)ctx.bias_ctrl.diag_error_dc_term,
                   (double)ctx.bias_ctrl.diag_error_obs_term,
                   (double)ctx.bias_ctrl.diag_error_spring_term,
                   (double)ctx.bias_ctrl.diag_dc_spring_offset_v);
            printf("Obs:   x=%+.4f y=%+.4f phi=%.4frad\r\n",
                   (double)ctx.bias_ctrl.obs_x,
                   (double)ctx.bias_ctrl.obs_y,
                   (double)ctx.bias_ctrl.phase_est_rad);
            printf("Hold:  active=%d lockst=%u\r\n",
                   ctx.bias_ctrl.hold_assist_active ? 1 : 0,
                   (unsigned)ctx.bias_ctrl.lock_streak);
            printf("LockD: obs=%d jump=%d rad=%d err=%d bias=%d phase=%d "
                   "r=%.3f berr=%+.4fV bwin=%.4fV\r\n",
                   ctx.bias_ctrl.diag_lock_observer_ok ? 1 : 0,
                   ctx.bias_ctrl.phase_jump_rejected ? 1 : 0,
                   ctx.bias_ctrl.diag_lock_radius_ok ? 1 : 0,
                   ctx.bias_ctrl.diag_lock_error_ok ? 1 : 0,
                   ctx.bias_ctrl.diag_lock_bias_ok ? 1 : 0,
                   ctx.bias_ctrl.diag_lock_phase_ok ? 1 : 0,
                   (double)ctx.bias_ctrl.diag_vector_radius,
                   (double)ctx.bias_ctrl.diag_bias_err_v,
                   (double)ctx.bias_ctrl.diag_bias_window_v);
        }
    } else if (strncmp(cmd, "set bp ", 7) == 0) {
        const char *bp = cmd + 7;
        if (strcmp(bp, "quad") == 0) {
            ctx.config->target_point = BIAS_POINT_QUAD;
            ctx.config->target_phase_rad = (float)M_PI / 2.0f;
        } else if (strcmp(bp, "max") == 0) {
            ctx.config->target_point = BIAS_POINT_MAX;
            ctx.config->target_phase_rad = (float)M_PI;
        } else if (strcmp(bp, "min") == 0) {
            ctx.config->target_point = BIAS_POINT_MIN;
            ctx.config->target_phase_rad = 0.0f;
        } else if (strncmp(bp, "custom ", 7) == 0) {
            /* "set bp custom <degrees>"  — arbitrary φ_code phase (0–180°),
             * where 0°=MIN, 90°=QUAD, 180°=MAX. */
            char *endptr = NULL;
            float deg = strtof(bp + 7, &endptr);
            if (endptr != bp + 7 && deg >= 0.0f && deg <= 180.0f) {
                ctx.config->target_point = BIAS_POINT_CUSTOM;
                float rad = deg * ((float)M_PI / 180.0f);
                ctx.config->target_phase_rad = rad;
                mzm_set_custom_phase(rad);
                if (ctx.config->cal_harmonics_valid) {
                    printf("[bp] custom %.1f deg (%.4f rad)\r\n",
                           (double)deg, (double)rad);
                } else {
                    printf("[bp] custom %.1f deg (%.4f rad)  [no harmonic-axis cal — run scan vpi first]\r\n",
                           (double)deg, (double)rad);
                }
            } else {
                printf("[bp] usage: set bp custom <degrees>  (0–180)\r\n");
            }
        }
        if (ctx.config->target_point != BIAS_POINT_CUSTOM) {
            printf("[bp] %s\r\n", bias_point_name(ctx.config->target_point));
        }
        bias_ctrl_set_target(&ctx.bias_ctrl, ctx.config->target_point);
    } else if (strcmp(cmd, "dac mid") == 0) {
        /* Set all 8 channels to 0 V output (mid-scale code 32768) */
        int ret = dac8568_write_channel(DAC8568_CH_ALL, 32768);
        printf("[dac] all channels -> 0.0 V, ret=%d\r\n", ret);
    } else if (strncmp(cmd, "dac test", 8) == 0) {
        /* "dac test <channels> <dc_v> [pilot_mv] [duration_s]"
         *
         * Output pilot tone on selected DAC channels with configurable amplitude.
         * Polls DRDY for 64 kHz pacing; reads ADC to reset DRDY each cycle.
         *
         * channels: "A"–"H", comma/space-free combo like "AB" or "ACE",
         *           or "ALL" / "*" for all 8 channels.
         * dc_v:     DC bias in volts (-10 … +10).  Default 0.0.
         * pilot_mv: pilot peak amplitude in mV (1 … 500).  Default 50.
         * duration: test duration in seconds (0.5 … 30).  Default 3.
         *
         * Examples:
         *   dac test A 0.0              →  CH_A, 0 V DC,  50 mV pilot, 3 s
         *   dac test AB 1.0 100 5      →  CH_A+B, 1 V DC, 100 mV pilot, 5 s
         *   dac test ALL 0.0 50        →  all 8 ch, 0 V DC, 50 mV pilot, 3 s */
        char   ch_buf[16];
        float  dc_v      = 0.0f;
        float  pilot_mv  = ctx.config->pilot_amplitude_v * 1000.0f;
        float  duration  = 3.0f;
        char  *endptr    = NULL;
        int    n_tokens  = 0;
        uint8_t ch_list[8];
        int    n_ch      = 0;

        /* ── token 1: channel selector ── */
        if (cmd[8] == ' ' && cmd[9] != '\0') {
            const char *p = cmd + 9;
            while (*p == ' ') p++;
            const char *tok = p;
            while (*p != ' ' && *p != '\0') p++;
            int tok_len = (int)(p - tok);
            if (tok_len > 0 && tok_len < (int)sizeof(ch_buf)) {
                memcpy(ch_buf, tok, (size_t)tok_len);
                ch_buf[tok_len] = '\0';
                n_tokens++;
                endptr = (char *)p;
            }
        }

        /* ── token 2: dc voltage ── */
        if (n_tokens >= 1 && endptr != NULL && *endptr != '\0') {
            while (*endptr == ' ') endptr++;
            if (*endptr != '\0') {
                dc_v = strtof(endptr, &endptr);
                n_tokens++;
            }
        }

        /* ── token 3 (optional): pilot amplitude mV ── */
        if (n_tokens >= 2 && endptr != NULL && *endptr != '\0') {
            while (*endptr == ' ') endptr++;
            if (*endptr != '\0' && *endptr != '-' && (*endptr < '0' || *endptr > '9')) {
                /* non-numeric → skip, maybe it was part of the channels string */
            } else if (*endptr != '\0') {
                char *inner = NULL;
                float v = strtof(endptr, &inner);
                if (inner != endptr) {
                    pilot_mv = v;
                    endptr   = inner;
                    n_tokens++;
                }
            }
        }

        /* ── token 4 (optional): duration seconds ── */
        if (n_tokens >= 3 && endptr != NULL && *endptr != '\0') {
            while (*endptr == ' ') endptr++;
            if (*endptr != '\0') {
                char *inner = NULL;
                float v = strtof(endptr, &inner);
                if (inner != endptr) {
                    duration = v;
                    n_tokens++;
                }
            }
        }

        /* ── clamp ── */
        if (dc_v < -10.0f) dc_v = -10.0f;
        if (dc_v >  10.0f) dc_v =  10.0f;
        if (pilot_mv < 1.0f)    pilot_mv = 1.0f;
        if (pilot_mv > 500.0f)  pilot_mv = 500.0f;
        if (duration < 0.5f)    duration = 0.5f;
        if (duration > 30.0f)   duration = 30.0f;

        /* ── resolve channels ── */
        if (n_tokens < 1 || ch_buf[0] == '\0') {
            ch_list[0] = DAC8568_CH_A;
            n_ch = 1;
        } else if (strcmp(ch_buf, "ALL") == 0 || strcmp(ch_buf, "*") == 0) {
            for (int i = 0; i < 8; i++) {
                ch_list[i] = (uint8_t)i;   /* CH_A=0 … CH_H=7 */
            }
            n_ch = 8;
        } else {
            for (const char *c = ch_buf; *c != '\0'; c++) {
                char letter = *c;
                if (letter >= 'a' && letter <= 'h') letter = (char)(letter - 'a' + 'A');
                if (letter >= 'A' && letter <= 'H' && n_ch < 8) {
                    ch_list[n_ch++] = (uint8_t)(letter - 'A');  /* A→0, H→7 */
                }
            }
            if (n_ch == 0) {
                ch_list[0] = DAC8568_CH_A;
                n_ch = 1;
            }
        }

        uint32_t n_samples = (uint32_t)(duration * (float)DSP_SAMPLE_RATE_HZ);

        pilot_gen_t pg;
        pilot_gen_init(&pg, (float)DSP_PILOT_FREQ_HZ,
                       (float)DSP_SAMPLE_RATE_HZ, pilot_mv * 0.001f);

        printf("[dac] test start: ch=");
        for (int ci = 0; ci < n_ch; ci++) {
            printf("%c%s", (char)('A' + ch_list[ci]), (ci + 1 < n_ch) ? "," : "");
        }
        printf(" dc=%+.3f V pilot=%.0f mV f0=%.0f Hz"
               " duration=%.1f s (%lu samples)\r\n",
               (double)dc_v, (double)pilot_mv,
               (double)DSP_PILOT_FREQ_HZ, (double)duration,
               (unsigned long)n_samples);

        float    min_dac_v = dc_v + pilot_mv * 0.001f;
        float    max_dac_v = dc_v - pilot_mv * 0.001f;
        uint32_t sent      = 0;

        for (uint32_t i = 0; i < n_samples; i++) {
            float pilot = pilot_gen_next(&pg);
            float dac_v = dc_v + pilot;

            for (int ci = 0; ci < n_ch; ci++) {
                dac8568_set_voltage(ch_list[ci], dac_v);
                /* Each channel fires a DMA transfer (~1.3 µs frame +
                 * ~300 ns ISR to clear s_dma_inflight).  Spin until
                 * completion so the next channel write (or the DRDY
                 * poll in the next cycle) never sees inflight=1. */
                uint32_t tw = HAL_GetTick();
                while (dac8568_send_raw_is_inflight()) {
                    if ((HAL_GetTick() - tw) > 1) break;
                }
            }

            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 10) {
                    printf("[dac] test ABORT: DRDY timeout after %lu samples\r\n",
                           (unsigned long)sent);
                    for (int ci = 0; ci < n_ch; ci++) {
                        dac8568_set_voltage(ch_list[ci], dc_v);
                        uint32_t tw = HAL_GetTick();
                        while (dac8568_send_raw_is_inflight()) {
                            if ((HAL_GetTick() - tw) > 1) break;
                        }
                    }
                    goto cmd_done;
                }
            }

            ads131m02_sample_t dummy;
            (void)ads131m02_read_sample(&dummy);

            if (dac_v < min_dac_v) min_dac_v = dac_v;
            if (dac_v > max_dac_v) max_dac_v = dac_v;
            sent++;
        }

        /* Restore DC level on all channels */
        for (int ci = 0; ci < n_ch; ci++) {
            dac8568_set_voltage(ch_list[ci], dc_v);
            uint32_t tw = HAL_GetTick();
            while (dac8568_send_raw_is_inflight()) {
                if ((HAL_GetTick() - tw) > 1) break;
            }
        }

        printf("[dac] test done: %lu samples sent, DAC range [%.3f, %.3f] V"
               " (pk-pk=%.0f mV)\r\n",
               (unsigned long)sent, (double)min_dac_v, (double)max_dac_v,
               (double)((max_dac_v - min_dac_v) * 1000.0f));
    } else if (strncmp(cmd, "dac ", 4) == 0) {
        /* "dac <voltage>"  — set DAC channel A to specified voltage (-10..+10 V).
         * Used for hardware verification with a multimeter.
         * Example: "dac 0.0" → subtractor output should read 0.0 V
         *          "dac 5.0" → subtractor output should read 5.0 V  */
        char *endptr = NULL;
        float v = strtof(cmd + 4, &endptr);
        if (endptr != cmd + 4) {
            if (v < -10.0f) v = -10.0f;
            if (v > 10.0f)  v = 10.0f;
            uint16_t code = board_voltage_to_dac_code(v);
            int ret = dac8568_write_channel(DAC8568_CH_A, code);
            if (ret == 0) {
                printf("[dac] CH_A set to %+.3f V (code=%u)\r\n", (double)v, code);
            } else {
                printf("[dac] SPI error %d\r\n", ret);
            }
        } else {
            printf("[dac] usage: dac <voltage>  (e.g. dac 0.0)\r\n");
        }
    } else if (strncmp(cmd, "adc", 3) == 0) {
        /* "adc [N]" — collect N consecutive DRDY-gated samples (default 64),
         * then print them all.
         *
         * Acquisition and printing are intentionally separated: interleaving
         * printf (blocking UART TX) with DRDY polling causes large gaps because
         * each printf line takes ~4 ms at 115200 baud while the ADC fires every
         * 15.6 µs at 64 kSPS, causing ~256 samples to be skipped per print.
         *
         * At 64 kSPS, 64 samples cover 1 ms — one full cycle of a 1 kHz sine.
         *
         * NOTE: Not available while controller is active (same SPI2 conflict as goertzel). */
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            printf("[adc] not available while controller is running (state=%s)\r\n",
                   app_state_name(ctx.state));
            goto cmd_done;
        }
        int n = 64;
        if (cmd[3] == ' ' && cmd[4] != '\0') {
            int parsed = atoi(cmd + 4);
            if (parsed > 0 && parsed <= 256) {
                n = parsed;
            }
        }

        /* Phase 1: Acquire all samples back-to-back (no printf here). */
        ads131m02_sample_t samples[256];
        int count = 0;
        bool drdy_timeout = false;

        for (int i = 0; i < n; i++) {
            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 5) {
                    drdy_timeout = true;
                    goto adc_collect_done;
                }
            }
            if (ads131m02_read_sample(&samples[count]) == 0) {
                count++;
            }
        }
        adc_collect_done:

        /* Phase 2: Print all collected samples. */
        printf("ADC samples (%d collected", count);
        if (drdy_timeout) {
            printf(", DRDY timeout");
        }
        printf("):\r\n");
        for (int i = 0; i < count; i++) {
            float v0 = ads131m02_code_to_voltage(samples[i].ch0, ADS131M02_GAIN_1);
            float v1 = ads131m02_code_to_voltage(samples[i].ch1, ADS131M02_GAIN_1);
            printf("  [%3d] CH0=%9ld (%+.6fV)  CH1=%9ld (%+.6fV)\r\n",
                   i, (long)samples[i].ch0, (double)v0,
                   (long)samples[i].ch1, (double)v1);
        }
    } else if (strncmp(cmd, "goertzel", 8) == 0) {
        /* "goertzel [N]" — run N Goertzel blocks (default 1), print H1/H2/DC per block.
         * CH0 (AC): extracts f0=1 kHz (H1) and 2f0=2 kHz (H2) via Goertzel.
         * CH1 (DC): block mean via dc_accum.
         * Block size = DSP_GOERTZEL_BLOCK_SIZE (10 full pilot cycles = 10 ms).
         *
         * NOTE: Cannot run while bias controller is active — both this command and
         * the continuous-mode ISR use SPI2; running them concurrently corrupts
         * transfers and hangs the bus. */
        if (ctx.state == APP_STATE_LOCKING || ctx.state == APP_STATE_LOCKED) {
            printf("[goertzel] not available while controller is running (state=%s)\r\n"
                   "  use 'stop' first, or 'status' to read last_harmonics\r\n",
                   app_state_name(ctx.state));
            goto cmd_done;
        }
        int n_blocks = 1;
        if (cmd[8] == ' ' && cmd[9] != '\0') {
            int parsed = atoi(cmd + 9);
            if (parsed > 0 && parsed <= 100) {
                n_blocks = parsed;
            }
        }

        goertzel_state_t g_f0, g_2f0;
        dc_accum_t dc;
        goertzel_init(&g_f0,  (float)DSP_PILOT_FREQ_HZ,        (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        goertzel_init(&g_2f0, (float)DSP_PILOT_FREQ_HZ * 2.0f, (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        dc_accum_init(&dc, DSP_GOERTZEL_BLOCK_SIZE);

        float h1_sum = 0.0f, h1_sq_sum = 0.0f;
        float h2_sum = 0.0f, h2_sq_sum = 0.0f;
        float dc_sum = 0.0f, dc_sq_sum = 0.0f;
        int blocks_done = 0;

        static const float INV_SQRT2 = 0.70710678f;
        static const float DBV_FLOOR = -120.0f;

        bool drdy_timeout = false;
        for (int b = 0; b < n_blocks && !drdy_timeout; b++) {
            goertzel_reset(&g_f0);
            goertzel_reset(&g_2f0);
            dc_accum_reset(&dc);

            for (uint32_t i = 0; i < DSP_GOERTZEL_BLOCK_SIZE; i++) {
                uint32_t t0 = HAL_GetTick();
                while (board_adc_drdy_read() != 0) {
                    if ((HAL_GetTick() - t0) > 5) {
                        drdy_timeout = true;
                        break;
                    }
                }
                if (drdy_timeout) { break; }

                ads131m02_sample_t s;
                if (ads131m02_read_sample(&s) == 0 && s.valid) {
                    float ch0 = ads131m02_code_to_voltage(s.ch0, ADS131M02_GAIN_1);
                    float ch1 = ads131m02_code_to_voltage(s.ch1, ADS131M02_GAIN_1);
                    goertzel_process_sample(&g_f0,  ch0);
                    goertzel_process_sample(&g_2f0, ch0);
                    dc_accum_process(&dc, ch1);
                }
            }

            if (drdy_timeout) {
                printf("[dsp] DRDY timeout\r\n");
                break;
            }

            float h1_mag, h1_phase_dummy, h2_mag, h2_phase_dummy;
            goertzel_get_result(&g_f0,  &h1_mag, &h1_phase_dummy);
            goertzel_get_result(&g_2f0, &h2_mag, &h2_phase_dummy);
            float dc_val = dc_accum_get_mean(&dc);

            h1_sum += h1_mag;
            h1_sq_sum += h1_mag * h1_mag;
            h2_sum += h2_mag;
            h2_sq_sum += h2_mag * h2_mag;
            dc_sum += dc_val;
            dc_sq_sum += dc_val * dc_val;
            blocks_done++;
        }

        if (blocks_done > 0) {
            float inv_blocks = 1.0f / (float)blocks_done;
            float h1_mean = h1_sum * inv_blocks;
            float h2_mean = h2_sum * inv_blocks;
            float dc_mean = dc_sum * inv_blocks;

            float h1_var = h1_sq_sum * inv_blocks - h1_mean * h1_mean;
            float h2_var = h2_sq_sum * inv_blocks - h2_mean * h2_mean;
            float dc_var = dc_sq_sum * inv_blocks - dc_mean * dc_mean;
            if (h1_var < 0.0f) { h1_var = 0.0f; }
            if (h2_var < 0.0f) { h2_var = 0.0f; }
            if (dc_var < 0.0f) { dc_var = 0.0f; }

            float h1_std = sqrtf(h1_var);
            float h2_std = sqrtf(h2_var);
            float dc_std = sqrtf(dc_var);
            float h1_mean_dbv = (h1_mean > 1e-6f) ? 20.0f * log10f(h1_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_mean_dbv = (h2_mean > 1e-6f) ? 20.0f * log10f(h2_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_rel_dbc = (h1_mean > 1e-9f && h2_mean > 1e-9f) ? 20.0f * log10f(h2_mean / h1_mean) : DBV_FLOOR;
            float window_ms = 1000.0f * (float)(blocks_done * DSP_GOERTZEL_BLOCK_SIZE) / (float)DSP_SAMPLE_RATE_HZ;

            printf("[dsp] mean %d blks (%.1f ms): H1=%+.6fV(%+.1fdBV) sigma=%.4fmV  "
                   "H2=%+.6fV(%+.1fdBV, %+.1fdBc) sigma=%.4fmV  DC=%+.6fV sigma=%.4fmV\r\n",
                   blocks_done, (double)window_ms,
                   (double)h1_mean, (double)h1_mean_dbv, (double)(h1_std * 1000.0f),
                   (double)h2_mean, (double)h2_mean_dbv, (double)h2_rel_dbc, (double)(h2_std * 1000.0f),
                   (double)dc_mean, (double)(dc_std * 1000.0f));
        }
    } else if (strncmp(cmd, "set mod ", 8) == 0) {
        const char *mod = cmd + 8;
        if (strcmp(mod, "mzm") == 0) {
            ctx.config->modulator_type = MOD_TYPE_MZM;
        }
        /* Future: add other modulator types */
        bias_ctrl_set_modulator(&ctx.bias_ctrl, ctx.config->modulator_type);

    } else if (strncmp(cmd, "set pilot ", 10) == 0) {
        /* "set pilot <mVpp>" — set pilot tone amplitude (peak-to-peak millivolts).
         * Default is 100 mVpp (0.05 V peak). Max is limited so bias + pilot stays
         * within the ±10 V DAC output range. */
        char *endptr = NULL;
        float mvpp = strtof(cmd + 10, &endptr);
        if (endptr == cmd + 10 || mvpp <= 0.0f) {
            printf("[pilot] usage: set pilot <mVpp>  (e.g. set pilot 100)\r\n");
        } else {
            float peak_v = mvpp / 2000.0f;           /* mVpp → V peak */
            float clamp_v = 10.0f - peak_v;
            if (clamp_v < 1.0f) {
                printf("[pilot] amplitude too large (max ~19800 mVpp)\r\n");
            } else {
                ctx.config->pilot_amplitude_v = peak_v;
                pilot_gen_set_amplitude(&ctx.bias_ctrl.pilot, peak_v);
                ctx.bias_ctrl.pilot_amplitude = peak_v;
                mzm_set_pilot_amplitude(peak_v);
                printf("[pilot] %.0f mVpp (peak=%.1f mV), scan clamp=+/-%.3f V\r\n",
                       (double)mvpp, (double)(peak_v * 1000.0f), (double)clamp_v);
            }
        }

    } else if (strncmp(cmd, "scan harmonics", 14) == 0) {
        /* "scan harmonics [fast] [blocks]" — open-loop harmonic scan for offline analysis.
         *
         * Prints one machine-readable line per step:
         *   HSCAN <bias_v> <h1_mag> <h1_phase> <h1_signed>
         *         <h2_mag> <h2_phase> <h2_signed> <dc_mean>
         *
         * The pilot is injected during the scan, but closed-loop bias control
         * remains stopped; this is a pure open-loop characterization sweep.
         */
        if (ctx.state != APP_STATE_IDLE) {
            printf("[scan] stop bias control first (state must be IDLE)\r\n");
            return;
        }

        bool fast_mode = (strstr(cmd + 14, "fast") != NULL);
        int n_blocks_per_step = 3;
        {
            const char *p = cmd + 14;
            while (*p != '\0') {
                if (*p >= '0' && *p <= '9') {
                    int parsed = atoi(p);
                    if (parsed >= 1 && parsed <= 20) {
                        n_blocks_per_step = parsed;
                        break;
                    }
                }
                p++;
            }
        }
        float pilot_peak = ctx.config->pilot_amplitude_v;
        float clamp_v    = 10.0f - pilot_peak;
        float scan_start = fast_mode ? 0.0f : -clamp_v;
        float scan_stop  = clamp_v;
        const float step = 0.1f;

        int n_steps = (int)((scan_stop - scan_start) / step) + 1;
        if (n_steps > SCAN_MAX_STEPS) {
            n_steps = SCAN_MAX_STEPS;
        }

        pilot_gen_t scan_pilot;
        pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                       (float)DSP_SAMPLE_RATE_HZ, pilot_peak);

        goertzel_state_t g_h1, g_h2;
        goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        goertzel_init(&g_h2, (float)(DSP_PILOT_FREQ_HZ * 2),
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

        float est_s = (float)n_steps * (float)n_blocks_per_step
                      * (float)DSP_GOERTZEL_BLOCK_SIZE / (float)DSP_SAMPLE_RATE_HZ;
        printf("[scan] harmonic scan: %+.2fV to %+.2fV, step=0.1V, "
               "%d blocks/step, est=%.0fs\r\n",
               (double)scan_start, (double)scan_stop,
               n_blocks_per_step, (double)est_s);

        dac8568_set_voltage(ctx.config->bias_dac_channel, scan_start);
        board_delay_ms(SCAN_INITIAL_SETTLE_MS);

        bool drdy_abort = false;
        for (int si = 0; si < n_steps && !drdy_abort; si++) {
            float bias_v = scan_start + (float)si * step;
            if (bias_v > scan_stop + step * 0.5f) {
                break;
            }

            dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
            board_delay_ms(2);

            if (si == 0) {
                goertzel_reset(&g_h1);
                goertzel_reset(&g_h2);
                pilot_gen_reset(&scan_pilot);
                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE && !drdy_abort; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }
                    ads131m02_sample_t smp;
                    (void)ads131m02_read_sample(&smp);
                }
                if (drdy_abort) { break; }
                dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
                board_delay_ms(2);
            }

            float h1_i_blocks[20];
            float h1_q_blocks[20];
            float h2_i_blocks[20];
            float h2_q_blocks[20];
            float dc_sum   = 0.0f;
            uint32_t dc_count = 0;

            for (int b = 0; b < n_blocks_per_step && !drdy_abort; b++) {
                goertzel_reset(&g_h1);
                goertzel_reset(&g_h2);
                pilot_gen_reset(&scan_pilot);

                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }

                    ads131m02_sample_t smp;
                    if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                        float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                        float ch1 = ads131m02_code_to_voltage(smp.ch1, ADS131M02_GAIN_1);
                        goertzel_process_sample(&g_h1, ch0);
                        goertzel_process_sample(&g_h2, ch0);
                        dc_sum += ch1;
                        dc_count++;
                    }
                }

                if (!drdy_abort) {
                    float h1_mag, h1_phase, h2_mag, h2_phase;
                    goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                    goertzel_get_result(&g_h2, &h2_mag, &h2_phase);
                    h1_i_blocks[b] = h1_mag * cosf(h1_phase);
                    h1_q_blocks[b] = h1_mag * sinf(h1_phase);
                    h2_i_blocks[b] = h2_mag * cosf(h2_phase);
                    h2_q_blocks[b] = h2_mag * sinf(h2_phase);
                }
            }

            if (drdy_abort) { break; }

            float h1_i = robust_meanf_local(h1_i_blocks, n_blocks_per_step);
            float h1_q = robust_meanf_local(h1_q_blocks, n_blocks_per_step);
            float h2_i = robust_meanf_local(h2_i_blocks, n_blocks_per_step);
            float h2_q = robust_meanf_local(h2_q_blocks, n_blocks_per_step);
            float h1_mag = sqrtf(h1_i * h1_i + h1_q * h1_q);
            float h2_mag = sqrtf(h2_i * h2_i + h2_q * h2_q);
            float h1_phase = atan2f(h1_q, h1_i);
            float h2_phase = atan2f(h2_q, h2_i);
            float h1_signed = h1_mag * cosf(h1_phase);
            float h2_signed = h2_mag * sinf(h2_phase);
            float dc_mean = (dc_count > 0) ? (dc_sum / (float)dc_count) : 0.0f;

            printf("HSCAN %+.3f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\r\n",
                   (double)bias_v,
                   (double)h1_mag, (double)h1_phase, (double)h1_signed,
                   (double)h2_mag, (double)h2_phase, (double)h2_signed,
                   (double)dc_mean);
        }

        dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);
        if (drdy_abort) {
            printf("[scan] DRDY timeout — harmonic scan aborted\r\n");
        } else {
            printf("[scan] harmonic scan done\r\n");
        }
    } else if (strcmp(cmd, "cal bias") == 0) {
        if (ctx.state != APP_STATE_IDLE) {
            printf("[cal] stop bias control first (state must be IDLE)\r\n");
        } else {
            bias_cal_result_t result = {0};
            if (run_bias_calibration_scan(&result, true)) {
                commit_bias_calibration(&result);
            }
        }
    } else if (strncmp(cmd, "scan vpi", 8) == 0) {
        /* "scan vpi [fast]" — open-loop V_pi characterization sweep.
         *
         * Sweeps bias from -clamp_v to +clamp_v in 0.1 V steps while injecting
         * the pilot tone, collects 3 Goertzel blocks per step (60 ms window +
         * 2 ms settle per step), and prints raw H1 magnitude for each step.
         * After the sweep, finds H1 minima by local-minimum detection with 10%
         * threshold and parabolic interpolation, then reports V_pi as the mean
         * inter-minimum spacing.
         *
         * "fast" modifier: single-sided 0 V → +clamp_v scan (~6 s vs ~12 s). */
        if (ctx.state != APP_STATE_IDLE) {
            printf("[scan] stop bias control first (state must be IDLE)\r\n");
            return;
        }

        bool fast_mode = (strstr(cmd + 8, "fast") != NULL);
        float pilot_peak = ctx.config->pilot_amplitude_v;
        float clamp_v    = 10.0f - pilot_peak;
        float scan_start = fast_mode ? 0.0f : -clamp_v;
        float scan_stop  = clamp_v;
        const float step = 0.1f;
        const int n_blocks_per_step = 3;

        int n_steps = (int)((scan_stop - scan_start) / step) + 1;
        if (n_steps > SCAN_MAX_STEPS) {
            n_steps = SCAN_MAX_STEPS;
        }

        static float scan_bias_buf[SCAN_MAX_STEPS];
        static float scan_h1_buf[SCAN_MAX_STEPS];

        /* Local DSP state — does not disturb closed-loop ctx.bias_ctrl */
        pilot_gen_t scan_pilot;
        pilot_gen_init(&scan_pilot, (float)DSP_PILOT_FREQ_HZ,
                       (float)DSP_SAMPLE_RATE_HZ, pilot_peak);

        goertzel_state_t g_h1;
        goertzel_init(&g_h1, (float)DSP_PILOT_FREQ_HZ,
                      (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);

        float est_s = (float)n_steps * (float)n_blocks_per_step
                      * (float)DSP_GOERTZEL_BLOCK_SIZE / (float)DSP_SAMPLE_RATE_HZ;
        printf("[scan] Vpi scan: %+.2fV to %+.2fV, step=0.1V, "
               "%d blocks/step, est=%.0fs\r\n",
               (double)scan_start, (double)scan_stop,
               n_blocks_per_step, (double)est_s);

        /* Pre-settle at scan start: the DAC was at 0V (restored after previous
         * scan), so jumping to scan_start may be a large step (~10V).  Give the
         * bias path time to settle before the first measurement. */
        dac8568_set_voltage(ctx.config->bias_dac_channel, scan_start);
        board_delay_ms(SCAN_INITIAL_SETTLE_MS);

        int steps_done = 0;
        bool drdy_abort = false;

        for (int si = 0; si < n_steps && !drdy_abort; si++) {
            float bias_v = scan_start + (float)si * step;
            if (bias_v > scan_stop + step * 0.5f) {
                break;
            }

            /* 2 ms settle per step.  The bias path RC (cable + electrode) may
             * be ~0.5-1 ms; 2 ms gives >4τ settling for a 0.1V step. */
            dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
            board_delay_ms(2);

            if (si == 0) {
                goertzel_reset(&g_h1);
                pilot_gen_reset(&scan_pilot);
                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE && !drdy_abort; s++) {
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }
                    ads131m02_sample_t smp;
                    (void)ads131m02_read_sample(&smp);
                }
                if (drdy_abort) { break; }
                dac8568_set_voltage(ctx.config->bias_dac_channel, bias_v);
                board_delay_ms(2);
            }

            /* Coherently accumulate H1 I/Q across n_blocks_per_step blocks */
            float h1_i_blocks[20];
            float h1_q_blocks[20];

            for (int b = 0; b < n_blocks_per_step && !drdy_abort; b++) {
                goertzel_reset(&g_h1);
                pilot_gen_reset(&scan_pilot);

                for (uint32_t s = 0; s < DSP_GOERTZEL_BLOCK_SIZE; s++) {
                    /* Write bias + pilot to DAC before reading ADC */
                    float dac_v = bias_v + pilot_gen_next(&scan_pilot);
                    dac8568_set_voltage(ctx.config->bias_dac_channel, dac_v);

                    /* Wait for DRDY */
                    uint32_t t0 = HAL_GetTick();
                    while (board_adc_drdy_read() != 0) {
                        if ((HAL_GetTick() - t0) > 5) {
                            drdy_abort = true;
                            break;
                        }
                    }
                    if (drdy_abort) { break; }

                    ads131m02_sample_t smp;
                    if (ads131m02_read_sample(&smp) == 0 && smp.valid) {
                        float ch0 = ads131m02_code_to_voltage(smp.ch0, ADS131M02_GAIN_1);
                        goertzel_process_sample(&g_h1, ch0);
                    }
                }

                if (!drdy_abort) {
                    float h1_mag, h1_phase;
                    goertzel_get_result(&g_h1, &h1_mag, &h1_phase);
                    h1_i_blocks[b] = h1_mag * cosf(h1_phase);
                    h1_q_blocks[b] = h1_mag * sinf(h1_phase);
                }
            }

            if (drdy_abort) { break; }

            float h1_i = robust_meanf_local(h1_i_blocks, n_blocks_per_step);
            float h1_q = robust_meanf_local(h1_q_blocks, n_blocks_per_step);
            float h1_avg = sqrtf(h1_i * h1_i + h1_q * h1_q);
            scan_bias_buf[steps_done] = bias_v;
            scan_h1_buf[steps_done]   = h1_avg;
            steps_done++;

            printf("SCAN %+.3f %.6f\r\n", (double)bias_v, (double)h1_avg);
        }

        /* Restore DAC to 0 V regardless of outcome */
        dac8568_set_voltage(ctx.config->bias_dac_channel, 0.0f);

        if (drdy_abort) {
            printf("[scan] DRDY timeout — scan aborted at step %d\r\n", steps_done);
            return;
        }

        /* ---- Vpi extraction via minimum (zero-crossing) detection ----
         *
         * H1 ∝ |sin(π·V/Vpi + φ)|: the minima are deep and sharp (H1 → 0 at
         * max/min transmission), while the maxima are broad and noisy.
         * Consecutive minima are spaced exactly Vpi apart, so minimum detection
         * gives a more accurate and robust Vpi estimate than peak detection. */
        float h1_max = 0.0f;
        for (int i = 0; i < steps_done; i++) {
            if (scan_h1_buf[i] > h1_max) { h1_max = scan_h1_buf[i]; }
        }

        if (h1_max < 1e-7f) {
            printf("[scan] no signal detected — check pilot and ADC connection\r\n");
            return;
        }

        /* Minimum threshold: below 10% of maximum H1 */
        float min_threshold = h1_max * 0.1f;
        float min_v[16];
        int   n_mins = 0;

        for (int i = 1; i < steps_done - 1 && n_mins < 16; i++) {
            if (scan_h1_buf[i] < scan_h1_buf[i - 1] &&
                scan_h1_buf[i] < scan_h1_buf[i + 1] &&
                scan_h1_buf[i] < min_threshold) {
                /* Parabolic interpolation for sub-step accuracy */
                float a = scan_h1_buf[i - 1];
                float b = scan_h1_buf[i];
                float c = scan_h1_buf[i + 1];
                float denom = a - 2.0f * b + c;
                float offset = (fabsf(denom) > 1e-12f) ? (0.5f * (a - c) / denom) : 0.0f;
                if (offset < -1.0f) { offset = -1.0f; }
                if (offset >  1.0f) { offset =  1.0f; }
                min_v[n_mins++] = scan_bias_buf[i] + offset * step;
            }
        }

        if (n_mins == 0) {
            printf("[scan] no minima found — scan range may be less than Vpi\r\n");
        } else if (n_mins == 1) {
            printf("[scan] 1 minimum at %+.3fV — widen range for Vpi measurement\r\n",
                   (double)min_v[0]);
        } else {
            float vpi_sum = 0.0f;
            for (int i = 0; i < n_mins - 1; i++) {
                vpi_sum += min_v[i + 1] - min_v[i];
            }
            float vpi = vpi_sum / (float)(n_mins - 1);

            printf("[scan] minima:");
            for (int i = 0; i < n_mins; i++) {
                printf(" %+.3fV", (double)min_v[i]);
            }
            printf("\r\n[scan] Vpi = %.3fV (%d intervals)\r\n",
                   (double)vpi, n_mins - 1);
            ctx.config->vpi_v = vpi;
        }
    } else if (strncmp(cmd, "perturb ", 8) == 0) {
        /* "perturb <V>" — add a one-shot offset to the bias voltage while
         * the controller is running, to test perturbation recovery.
         * The controller immediately starts correcting the error. */
        if (ctx.state != APP_STATE_LOCKING && ctx.state != APP_STATE_LOCKED) {
            printf("[perturb] only valid while controller is running\r\n");
        } else {
            char *endptr = NULL;
            float delta = strtof(cmd + 8, &endptr);
            if (endptr != cmd + 8) {
                ctx.bias_ctrl.bias_voltage += delta;
                ctx.bias_ctrl.locked = false;
                printf("[perturb] bias offset %+.3fV -> bias now %.3fV\r\n",
                       (double)delta, (double)ctx.bias_ctrl.bias_voltage);
            } else {
                printf("[perturb] usage: perturb <delta_V>  (e.g. perturb 3.0)\r\n");
            }
        }
    }

cmd_done:;
}
