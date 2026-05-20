#include "app_main_dpmzm.h"
#include "ctrl_auto_dpmzm.h"
#include "ctrl_lock_dpmzm.h"
#include "dsp_types.h"
#include "ctrl_scan_dpmzm.h"
#include "drv_ads131m02.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static app_dpmzm_context_t s_dpmzm_ctx;
static float s_pilot_i_phase_rad = 0.0f;
static float s_pilot_q_phase_rad = 0.0f;
static bool s_scan_pilot_active = false;
static bool s_p_bias_dirty = false;
static float s_pending_p_bias_v = 0.0f;
static bool s_scan_restore_valid = false;
static float s_scan_restore_bias_i_v = 0.0f;
static float s_scan_restore_bias_q_v = 0.0f;
static float s_scan_restore_bias_p_v = 0.0f;

#define DPMZM_PILOT_LUT_SIZE            1024U
#define DPMZM_PILOT_DMA_MAX_FRAMES      3U
#define DPMZM_PILOT_DAC_FRAME_BYTES     4U
#define DPMZM_CAPTURE_SAMPLES_MAX       8192U
#define DPMZM_CAPTURE_SETTLE_DEFAULT_MS 20U
#define DPMZM_CAPTURE_DRDY_TIMEOUT_MS   5U
#define DPMZM_LOCK_DEFAULT_DELTA_V      0.01f
#define DPMZM_LOCK_DEFAULT_GAIN_V       0.003f
#define DPMZM_LOCK_DEFAULT_MAX_STEP_V   0.003f
#define DPMZM_LOCK_DEFAULT_Q_GAIN_V     0.003f
#define DPMZM_LOCK_DEFAULT_Q_MAX_STEP_V 0.003f
#define DPMZM_LOCK_DEFAULT_DEADBAND     0.03f
#define DPMZM_LOCK_DEFAULT_SETTLE_MS    10U
#define DPMZM_LOCK_DEFAULT_IQ_BLOCKS    4U
#define DPMZM_LOCK_DEFAULT_P_BLOCKS     10U
#define DPMZM_LOCK_DEFAULT_IQ_WINDOW_V  0.20f
#define DPMZM_LOCK_DEFAULT_P_WINDOW_V   0.30f
#define DPMZM_LOCK_DEFAULT_INTERVAL_MS  500U
#define DPMZM_LOCK_ADAPTIVE_HARD_MAX_STEP_V 0.010f
#define DPMZM_LOCK_ADAPTIVE_MIN_SCALE       0.50f
#define DPMZM_LOCK_ADAPTIVE_MAX_SCALE       4.00f
#define DPMZM_LOCK_ADAPTIVE_GROWTH          1.50f
#define DPMZM_LOCK_ADAPTIVE_SHRINK          0.50f
#define DPMZM_LOCK_ADAPTIVE_SAME_DIR_COUNT  3U
#define DPMZM_LOCK_BOUNDARY_DEGRADE_DB      8.0f
#define DPMZM_LOCK_BOUNDARY_COUNT           3U
#define DPMZM_LOCK_BOUNDARY_WINDOW_V        0.15f
#define DPMZM_LOCK_BOUNDARY_STEP_V          0.01f
#define DPMZM_LOCK_BOUNDARY_COOLDOWN_STEPS  12
#define DPMZM_LOCK_BOUNDARY_EXPAND_MARGIN_V 0.10f
#define DPMZM_LOCK_BOUNDARY_FAST_REPEAT     30U
#define DPMZM_LOCK_BOUNDARY_FAST_EXPAND     1.50f
#define DPMZM_LOCK_BOUNDARY_MAX_WINDOW_V    0.80f
#define DPMZM_LOCK_BOUNDARY_SHRINK_STABLE   50U
#define DPMZM_LOCK_BOUNDARY_SHRINK          0.90f
#define DPMZM_LOCK_DC_GUARD_THRESHOLD_DB    4.0f
#define DPMZM_LOCK_DC_GUARD_RELEASE_DB      2.0f
#define DPMZM_LOCK_DC_GUARD_AVG_WINDOW      5U
#define DPMZM_LOCK_DC_GUARD_COUNT           3U
#define DPMZM_LOCK_DC_GUARD_COOLDOWN_STEPS  12
#define DPMZM_LOCK_DC_GUARD_FLOOR_V         1.0e-5f
#define DPMZM_LOCK_DC_RECHECK_IQ_WINDOW_V   0.08f
#define DPMZM_LOCK_DC_RECHECK_P_WINDOW_V    0.10f
#define DPMZM_LOCK_DC_RECHECK_STEP_V        0.01f
#define DPMZM_LOCK_DBM_FLOOR_MW             1.0e-15f

/*
 * The pilot phase accumulator must use TIM6's actual update rate. A hardcoded
 * 16 kHz assumption caused 1 kHz / 1.2 kHz pilots to shift when the MCU clock
 * tree produced a different TIM6 input clock.
 */
#define DPMZM_PILOT_TIM6_RATE_FALLBACK_HZ 16000.0f

static bool s_pilot_timer_running = false;
static volatile float s_pilot_timer_rate_hz = DPMZM_PILOT_TIM6_RATE_FALLBACK_HZ;
static bool s_pilot_lut_ready = false;
static float s_pilot_lut[DPMZM_PILOT_LUT_SIZE + 1U];
static uint8_t s_pilot_dma_frames[DPMZM_PILOT_DMA_MAX_FRAMES][DPMZM_PILOT_DAC_FRAME_BYTES];
static volatile bool s_pilot_dma_active = false;
static volatile uint8_t s_pilot_dma_frame_count = 0U;
static volatile uint8_t s_pilot_dma_frame_index = 0U;
static volatile bool s_pilot_dma_contains_p = false;
static float s_pilot_dma_pending_p_v = 0.0f;
static volatile uint32_t s_pilot_dma_drop_count = 0U;
static volatile uint32_t s_pilot_dma_error_count = 0U;
static volatile uint32_t s_pilot_tim6_irq_count = 0U;
static volatile uint32_t s_pilot_dma_schedule_count = 0U;
static uint32_t s_pilot_diag_start_ms = 0U;
static bool s_capture_pilot_active = false;
static bool s_scan_reused_continuous_pilot = false;
static bool s_scan_started_temporary_pilot = false;
static uint32_t s_lock_last_cycle_ms = 0U;
static bool s_lock_cycle_busy = false;

typedef struct {
    int32_t ch0_code;
    int32_t ch1_code;
} dpmzm_capture_sample_t;

typedef struct {
    float scale;
    int8_t last_direction;
    uint8_t same_direction_count;
} dpmzm_lock_adaptive_axis_t;

typedef struct {
    bool valid;
    float best_objective;
    float best_dbm;
    float best_bias_v;
    float window_v;
    uint8_t clamp_streak;
    uint16_t stable_steps;
    int32_t cooldown_steps;
    uint32_t last_recheck_step;
} dpmzm_lock_boundary_axis_t;

typedef struct {
    bool anchor_valid;
    bool armed;
    float anchor_dc_v;
    float rise_history_db[DPMZM_LOCK_DC_GUARD_AVG_WINDOW];
    uint8_t history_count;
    uint8_t history_index;
    uint8_t trigger_count;
    int32_t cooldown_steps;
} dpmzm_lock_dc_guard_t;

typedef struct {
    bool valid;
    float sweep_v;
    float metric;
    float objective;
    float dbm;
    float dc_v;
} dpmzm_lock_local_measurement_t;

static dpmzm_capture_sample_t s_capture_samples[DPMZM_CAPTURE_SAMPLES_MAX];
static dpmzm_lock_adaptive_axis_t s_lock_adaptive[DPMZM_LOCK_AXIS_COUNT];
static dpmzm_lock_boundary_axis_t s_lock_boundary[DPMZM_LOCK_AXIS_COUNT];
static dpmzm_lock_dc_guard_t s_lock_dc_guard;

static float app_clampf(float value, float min_v, float max_v)
{
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

static float app_metric_to_dbm(float vpeak)
{
    const float vrms = fmaxf(vpeak, 0.0f) * 0.70710678f;
    float power_mw = ((vrms * vrms) / 50.0f) * 1000.0f;

    if (power_mw < DPMZM_LOCK_DBM_FLOOR_MW) {
        power_mw = DPMZM_LOCK_DBM_FLOOR_MW;
    }
    return 10.0f * log10f(power_mw);
}

static float app_objective_from_metric(float metric)
{
    metric = fmaxf(metric, 0.0f);
    return metric * metric;
}

static int8_t app_step_direction(float step_v)
{
    if (step_v > 1.0e-9f) {
        return 1;
    }
    if (step_v < -1.0e-9f) {
        return -1;
    }
    return 0;
}

static float lock_axis_base_gain(dpmzm_lock_axis_t axis)
{
    if (axis == DPMZM_LOCK_AXIS_Q) {
        return DPMZM_LOCK_DEFAULT_Q_GAIN_V;
    }
    return DPMZM_LOCK_DEFAULT_GAIN_V;
}

static float lock_axis_base_max_step(dpmzm_lock_axis_t axis)
{
    if (axis == DPMZM_LOCK_AXIS_Q) {
        return DPMZM_LOCK_DEFAULT_Q_MAX_STEP_V;
    }
    return DPMZM_LOCK_DEFAULT_MAX_STEP_V;
}

static void reset_lock_runtime_state(void)
{
    for (uint32_t i = 0U; i < (uint32_t)DPMZM_LOCK_AXIS_COUNT; i++) {
        s_lock_adaptive[i].scale = 1.0f;
        s_lock_adaptive[i].last_direction = 0;
        s_lock_adaptive[i].same_direction_count = 0U;

        memset(&s_lock_boundary[i], 0, sizeof(s_lock_boundary[i]));
        s_lock_boundary[i].window_v = DPMZM_LOCK_DEFAULT_IQ_WINDOW_V;
    }
    memset(&s_lock_dc_guard, 0, sizeof(s_lock_dc_guard));
}

static float lock_axis_dynamic_window(dpmzm_lock_axis_t axis)
{
    if (axis == DPMZM_LOCK_AXIS_P) {
        return DPMZM_LOCK_DEFAULT_P_WINDOW_V;
    }
    if (axis < DPMZM_LOCK_AXIS_COUNT && s_lock_boundary[axis].window_v > 0.0f) {
        return s_lock_boundary[axis].window_v;
    }
    return DPMZM_LOCK_DEFAULT_IQ_WINDOW_V;
}

static void update_lock_adaptive_state(dpmzm_lock_axis_t axis,
                                       float applied_step_v,
                                       bool held,
                                       bool clamped)
{
    dpmzm_lock_adaptive_axis_t *state;
    int8_t direction;

    if (axis >= DPMZM_LOCK_AXIS_COUNT) {
        return;
    }

    state = &s_lock_adaptive[axis];
    direction = app_step_direction(applied_step_v);

    if (direction == 0 || held || clamped) {
        state->same_direction_count = 0U;
        if (state->scale > 1.0f) {
            state->scale = fmaxf(1.0f, state->scale * DPMZM_LOCK_ADAPTIVE_SHRINK);
        }
        return;
    }

    if (state->last_direction != 0 && direction != state->last_direction) {
        state->same_direction_count = 1U;
        state->scale = fmaxf(DPMZM_LOCK_ADAPTIVE_MIN_SCALE,
                             state->scale * DPMZM_LOCK_ADAPTIVE_SHRINK);
    } else {
        state->same_direction_count++;
        if (state->same_direction_count >= DPMZM_LOCK_ADAPTIVE_SAME_DIR_COUNT) {
            state->scale = fminf(DPMZM_LOCK_ADAPTIVE_MAX_SCALE,
                                 state->scale * DPMZM_LOCK_ADAPTIVE_GROWTH);
        }
    }

    state->last_direction = direction;
}

static float lock_axis_current_bias(dpmzm_lock_axis_t axis)
{
    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return s_dpmzm_ctx.bias_i_v;
    case DPMZM_LOCK_AXIS_Q:
        return s_dpmzm_ctx.bias_q_v;
    case DPMZM_LOCK_AXIS_P:
        return s_dpmzm_ctx.bias_p_v;
    default:
        return 0.0f;
    }
}

static float lock_axis_anchor_bias(const dpmzm_lock_context_t *ctx,
                                   dpmzm_lock_axis_t axis)
{
    if (ctx == NULL || !ctx->anchor_valid) {
        return lock_axis_current_bias(axis);
    }
    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return ctx->anchor_i_v;
    case DPMZM_LOCK_AXIS_Q:
        return ctx->anchor_q_v;
    case DPMZM_LOCK_AXIS_P:
        return ctx->anchor_p_v;
    default:
        return lock_axis_current_bias(axis);
    }
}

static int apply_lock_axis_bias(dpmzm_lock_axis_t axis, float value)
{
    float vi = s_dpmzm_ctx.bias_i_v;
    float vq = s_dpmzm_ctx.bias_q_v;
    float vp = s_dpmzm_ctx.bias_p_v;

    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        vi = value;
        break;
    case DPMZM_LOCK_AXIS_Q:
        vq = value;
        break;
    case DPMZM_LOCK_AXIS_P:
        vp = value;
        break;
    default:
        return -1;
    }

    return app_dpmzm_scan_apply_bias_triplet(vi, vq, vp);
}

static void lock_set_stage_target(dpmzm_scan_request_t *req,
                                  dpmzm_lock_axis_t axis)
{
    if (req == NULL) {
        return;
    }

    if (axis == DPMZM_LOCK_AXIS_P) {
        req->stage = DPMZM_SCAN_STAGE_QTP;
        req->target = DPMZM_SCAN_TARGET_P;
    } else if (axis == DPMZM_LOCK_AXIS_Q) {
        req->stage = DPMZM_SCAN_STAGE_MITP;
        req->target = DPMZM_SCAN_TARGET_Q;
    } else {
        req->stage = DPMZM_SCAN_STAGE_MITP;
        req->target = DPMZM_SCAN_TARGET_I;
    }
}

static float lock_metric_for_axis(dpmzm_lock_axis_t axis,
                                  const dpmzm_measurement_t *m)
{
    if (m == NULL) {
        return 0.0f;
    }
    switch (axis) {
    case DPMZM_LOCK_AXIS_I:
        return m->mag_fi;
    case DPMZM_LOCK_AXIS_Q:
        return m->mag_fq;
    case DPMZM_LOCK_AXIS_P:
        return m->mag_fsum;
    default:
        return 0.0f;
    }
}

static bool pilot_generation_active_internal(void)
{
    return s_dpmzm_ctx.initialized &&
           (s_dpmzm_ctx.pilot_output_enabled || s_scan_pilot_active || s_capture_pilot_active) &&
           s_dpmzm_ctx.config->pilot_source == DPMZM_PILOT_SOURCE_ONBOARD;
}

static bool timer_owns_dac_outputs(void)
{
    return s_pilot_timer_running && pilot_generation_active_internal();
}

static float compute_tim6_update_rate_hz(void)
{
    RCC_ClkInitTypeDef clock_config;
    uint32_t flash_latency = 0U;
    uint32_t pclk1_hz = HAL_RCC_GetPCLK1Freq();
    uint32_t tim6_clk_hz = pclk1_hz;
    uint32_t prescaler_div = htim6.Init.Prescaler + 1U;
    uint32_t period_div = htim6.Init.Period + 1U;

    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
    if (clock_config.APB1CLKDivider != RCC_HCLK_DIV1) {
        tim6_clk_hz = pclk1_hz * 2U;
    }

    if (tim6_clk_hz == 0U || prescaler_div == 0U || period_div == 0U) {
        return DPMZM_PILOT_TIM6_RATE_FALLBACK_HZ;
    }

    return (float)((double)tim6_clk_hz /
                   ((double)prescaler_div * (double)period_div));
}

static void refresh_pilot_timer_rate(void)
{
    s_pilot_timer_rate_hz = compute_tim6_update_rate_hz();
}

static void init_pilot_lut_once(void)
{
    if (s_pilot_lut_ready) {
        return;
    }

    for (uint32_t i = 0; i < DPMZM_PILOT_LUT_SIZE; i++) {
        float phase = (2.0f * M_PI * (float)i) / (float)DPMZM_PILOT_LUT_SIZE;
        s_pilot_lut[i] = sinf(phase);
    }
    s_pilot_lut[DPMZM_PILOT_LUT_SIZE] = s_pilot_lut[0];
    s_pilot_lut_ready = true;
}

static void wrap_phase(float *phase_rad)
{
    if (phase_rad == NULL) {
        return;
    }
    if (*phase_rad >= 2.0f * M_PI || *phase_rad <= -2.0f * M_PI) {
        *phase_rad = fmodf(*phase_rad, 2.0f * M_PI);
    }
    if (*phase_rad < 0.0f) {
        *phase_rad += 2.0f * M_PI;
    }
}

static float lut_sample_from_phase(float phase_rad)
{
    float lut_index;
    uint32_t index0;
    float frac;
    float v0;
    float v1;

    wrap_phase(&phase_rad);
    init_pilot_lut_once();

    lut_index = phase_rad * ((float)DPMZM_PILOT_LUT_SIZE / (2.0f * M_PI));
    index0 = (uint32_t)lut_index;
    if (index0 >= DPMZM_PILOT_LUT_SIZE) {
        index0 = 0U;
    }
    frac = lut_index - (float)index0;
    v0 = s_pilot_lut[index0];
    v1 = s_pilot_lut[index0 + 1U];
    return v0 + (v1 - v0) * frac;
}

static uint32_t build_dac_frame(uint8_t command, uint8_t channel, uint16_t code)
{
    uint32_t frame = 0U;

    frame |= ((uint32_t)(command & 0x0FU)) << 24;
    frame |= ((uint32_t)(channel & 0x0FU)) << 20;
    frame |= ((uint32_t)code) << 4;
    return frame;
}

static void encode_dac_frame(uint8_t command, uint8_t channel, float voltage_v,
                             uint8_t frame_bytes[DPMZM_PILOT_DAC_FRAME_BYTES])
{
    uint32_t frame = build_dac_frame(command, channel,
                                     board_voltage_to_dac_code(voltage_v));

    frame_bytes[0] = (uint8_t)(frame >> 24);
    frame_bytes[1] = (uint8_t)(frame >> 16);
    frame_bytes[2] = (uint8_t)(frame >> 8);
    frame_bytes[3] = (uint8_t)(frame);
}

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
    s_p_bias_dirty = false;
    s_pending_p_bias_v = s_dpmzm_ctx.bias_p_v;
    return ret_p;
}

static int apply_drive(float tone_i_v, float tone_q_v)
{
    int ret_i = dac8568_set_voltage(s_dpmzm_ctx.config->bias_i_dac_channel,
                                    s_dpmzm_ctx.bias_i_v + tone_i_v);
    int ret_q = dac8568_set_voltage(s_dpmzm_ctx.config->bias_q_dac_channel,
                                    s_dpmzm_ctx.bias_q_v + tone_q_v);

    if (ret_i != 0) {
        return ret_i;
    }
    if (ret_q != 0) {
        return ret_q;
    }
    return 0;
}

static int apply_current_drive(void)
{
    float tone_i_v = 0.0f;
    float tone_q_v = 0.0f;

    if (s_dpmzm_ctx.config->pilot_source == DPMZM_PILOT_SOURCE_ONBOARD &&
        (s_dpmzm_ctx.pilot_output_enabled || s_scan_pilot_active)) {
        tone_i_v = s_dpmzm_ctx.config->pilot_i_amp_v * lut_sample_from_phase(s_pilot_i_phase_rad);
        tone_q_v = s_dpmzm_ctx.config->pilot_q_amp_v * lut_sample_from_phase(s_pilot_q_phase_rad);
    }

    return apply_drive(tone_i_v, tone_q_v);
}

static void update_bias_triplet_state(float vi, float vq, float vp)
{
    __disable_irq();
    s_dpmzm_ctx.bias_i_v = vi;
    s_dpmzm_ctx.bias_q_v = vq;
    s_dpmzm_ctx.bias_p_v = vp;
    s_pending_p_bias_v = vp;
    s_p_bias_dirty = true;
    __enable_irq();
}

static void update_single_bias_state(char path, float value)
{
    __disable_irq();
    switch (path) {
    case 'i':
        s_dpmzm_ctx.bias_i_v = value;
        break;
    case 'q':
        s_dpmzm_ctx.bias_q_v = value;
        break;
    case 'p':
        s_dpmzm_ctx.bias_p_v = value;
        s_pending_p_bias_v = value;
        s_p_bias_dirty = true;
        break;
    default:
        break;
    }
    __enable_irq();
}

static bool consume_pending_p_bias(float *vp_out)
{
    bool dirty = false;

    if (vp_out == NULL) {
        return false;
    }

    __disable_irq();
    dirty = s_p_bias_dirty;
    if (dirty) {
        *vp_out = s_pending_p_bias_v;
        s_p_bias_dirty = false;
    }
    __enable_irq();

    return dirty;
}

static void capture_scan_restore_biases(void)
{
    __disable_irq();
    s_scan_restore_bias_i_v = s_dpmzm_ctx.bias_i_v;
    s_scan_restore_bias_q_v = s_dpmzm_ctx.bias_q_v;
    s_scan_restore_bias_p_v = s_dpmzm_ctx.bias_p_v;
    s_scan_restore_valid = true;
    __enable_irq();
}

static void restore_scan_biases_to_context(void)
{
    __disable_irq();
    if (s_scan_restore_valid) {
        s_dpmzm_ctx.bias_i_v = s_scan_restore_bias_i_v;
        s_dpmzm_ctx.bias_q_v = s_scan_restore_bias_q_v;
        s_dpmzm_ctx.bias_p_v = s_scan_restore_bias_p_v;
        s_pending_p_bias_v = s_scan_restore_bias_p_v;
        s_p_bias_dirty = false;
        s_scan_restore_valid = false;
    }
    __enable_irq();
}

static void requeue_pending_p_bias(float vp)
{
    __disable_irq();
    s_pending_p_bias_v = vp;
    s_p_bias_dirty = true;
    __enable_irq();
}

static void reset_pilot_phases(void)
{
    s_pilot_i_phase_rad = 0.0f;
    s_pilot_q_phase_rad = 0.0f;
}

static void reset_pilot_diag_counters(void)
{
    __disable_irq();
    s_pilot_tim6_irq_count = 0U;
    s_pilot_dma_schedule_count = 0U;
    s_pilot_dma_drop_count = 0U;
    s_pilot_dma_error_count = 0U;
    __enable_irq();

    s_pilot_diag_start_ms = HAL_GetTick();
}

static void stop_pilot_timer(void)
{
    if (s_pilot_timer_running) {
        (void)HAL_TIM_Base_Stop_IT(&htim6);
        s_pilot_timer_running = false;
    }

    if (s_pilot_dma_active || hspi1.State != HAL_SPI_STATE_READY) {
        (void)HAL_SPI_Abort(&hspi1);
    }

    board_dac_cs_high();
    __disable_irq();
    s_pilot_dma_active = false;
    s_pilot_dma_frame_count = 0U;
    s_pilot_dma_frame_index = 0U;
    s_pilot_dma_contains_p = false;
    __enable_irq();
}

static bool start_pilot_timer(void)
{
    if (!pilot_generation_active_internal()) {
        return false;
    }

    init_pilot_lut_once();
    refresh_pilot_timer_rate();

    if (s_pilot_timer_running) {
        __HAL_TIM_SET_COUNTER(&htim6, 0U);
        __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
        return true;
    }

    if (htim6.State != HAL_TIM_STATE_READY) {
        (void)HAL_TIM_Base_Stop_IT(&htim6);
    }

    __HAL_TIM_SET_COUNTER(&htim6, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
    reset_pilot_diag_counters();
    if (HAL_TIM_Base_Start_IT(&htim6) == HAL_OK) {
        s_pilot_timer_running = true;
        return true;
    } else {
        s_pilot_timer_running = false;
        printf("[dpmzm] WARN: failed to start TIM6 pilot timer (state=%lu)\r\n",
               (unsigned long)htim6.State);
        return false;
    }
}

static bool start_spi1_dma_frame(uint8_t frame_index)
{
    HAL_StatusTypeDef status;

    board_dac_cs_low();
    status = HAL_SPI_Transmit_DMA(&hspi1,
                                  s_pilot_dma_frames[frame_index],
                                  DPMZM_PILOT_DAC_FRAME_BYTES);
    if (status != HAL_OK) {
        board_dac_cs_high();
        return false;
    }
    return true;
}

static bool schedule_pilot_dma_sequence(float tone_i_v, float tone_q_v,
                                        bool include_p_frame, float pending_p_v)
{
    uint8_t frame_count = 0U;

    if (s_pilot_dma_active) {
        s_pilot_dma_drop_count++;
        return false;
    }

    if (include_p_frame) {
        encode_dac_frame(DAC8568_CMD_WRITE_REG,
                         s_dpmzm_ctx.config->bias_p_dac_channel,
                         pending_p_v,
                         s_pilot_dma_frames[frame_count]);
        frame_count++;
    }

    encode_dac_frame(DAC8568_CMD_WRITE_REG,
                     s_dpmzm_ctx.config->bias_i_dac_channel,
                     s_dpmzm_ctx.bias_i_v + tone_i_v,
                     s_pilot_dma_frames[frame_count]);
    frame_count++;

    encode_dac_frame(DAC8568_CMD_WRITE_REG,
                     s_dpmzm_ctx.config->bias_q_dac_channel,
                     s_dpmzm_ctx.bias_q_v + tone_q_v,
                     s_pilot_dma_frames[frame_count]);
    frame_count++;

    __disable_irq();
    s_pilot_dma_active = true;
    s_pilot_dma_frame_count = frame_count;
    s_pilot_dma_frame_index = 0U;
    s_pilot_dma_contains_p = include_p_frame;
    s_pilot_dma_pending_p_v = pending_p_v;
    __enable_irq();

    if (!start_spi1_dma_frame(0U)) {
        __disable_irq();
        s_pilot_dma_active = false;
        s_pilot_dma_frame_count = 0U;
        s_pilot_dma_frame_index = 0U;
        s_pilot_dma_contains_p = false;
        __enable_irq();
        s_pilot_dma_error_count++;
        if (include_p_frame) {
            requeue_pending_p_bias(pending_p_v);
        }
        return false;
    }

    return true;
}

static void drive_next_sample_internal(void)
{
    float tone_i_v = 0.0f;
    float tone_q_v = 0.0f;
    float timer_rate_hz = s_pilot_timer_rate_hz;
    float phase_step_i;
    float phase_step_q;
    float pending_p_v = 0.0f;
    bool include_p_frame = false;

    if (!pilot_generation_active_internal()) {
        return;
    }

    if (timer_rate_hz <= 0.0f) {
        timer_rate_hz = DPMZM_PILOT_TIM6_RATE_FALLBACK_HZ;
    }

    include_p_frame = consume_pending_p_bias(&pending_p_v);
    tone_i_v = s_dpmzm_ctx.config->pilot_i_amp_v * lut_sample_from_phase(s_pilot_i_phase_rad);
    tone_q_v = s_dpmzm_ctx.config->pilot_q_amp_v * lut_sample_from_phase(s_pilot_q_phase_rad);
    if (!schedule_pilot_dma_sequence(tone_i_v, tone_q_v, include_p_frame, pending_p_v)) {
        return;
    }
    s_pilot_dma_schedule_count++;

    phase_step_i = (2.0f * M_PI * s_dpmzm_ctx.config->pilot_i_freq_hz) /
                   timer_rate_hz;
    phase_step_q = (2.0f * M_PI * s_dpmzm_ctx.config->pilot_q_freq_hz) /
                   timer_rate_hz;
    s_pilot_i_phase_rad += phase_step_i;
    s_pilot_q_phase_rad += phase_step_q;
    wrap_phase(&s_pilot_i_phase_rad);
    wrap_phase(&s_pilot_q_phase_rad);
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
    const char *pilot_output_mode =
        s_dpmzm_ctx.pilot_output_enabled
            ? (s_pilot_timer_running ? "continuous" : "continuous-failed")
            : "scan-only";
    float pilot_timer_rate_hz = compute_tim6_update_rate_hz();
    uint16_t code_i = board_voltage_to_dac_code(s_dpmzm_ctx.bias_i_v);
    uint16_t code_q = board_voltage_to_dac_code(s_dpmzm_ctx.bias_q_v);
    uint16_t code_p = board_voltage_to_dac_code(s_dpmzm_ctx.bias_p_v);
    uint32_t diag_now_ms = HAL_GetTick();
    uint32_t diag_elapsed_ms = 0U;
    uint32_t diag_irq_count = 0U;
    uint32_t diag_schedule_count = 0U;
    float diag_irq_rate_hz = 0.0f;
    float diag_schedule_rate_hz = 0.0f;

    s_pilot_timer_rate_hz = pilot_timer_rate_hz;
    if (s_pilot_diag_start_ms != 0U) {
        diag_elapsed_ms = diag_now_ms - s_pilot_diag_start_ms;
    }
    __disable_irq();
    diag_irq_count = s_pilot_tim6_irq_count;
    diag_schedule_count = s_pilot_dma_schedule_count;
    __enable_irq();
    if (diag_elapsed_ms > 0U) {
        diag_irq_rate_hz = ((float)diag_irq_count * 1000.0f) /
                           (float)diag_elapsed_ms;
        diag_schedule_rate_hz = ((float)diag_schedule_count * 1000.0f) /
                                (float)diag_elapsed_ms;
    }

    printf("[dpmzm] status\r\n");
    printf("  initialized: %s\r\n", s_dpmzm_ctx.initialized ? "yes" : "no");
    printf("  bias channels: I=%u Q=%u P=%u\r\n",
           (unsigned)s_dpmzm_ctx.config->bias_i_dac_channel,
           (unsigned)s_dpmzm_ctx.config->bias_q_dac_channel,
           (unsigned)s_dpmzm_ctx.config->bias_p_dac_channel);
    printf("  bias target:   I=%+.3fV Q=%+.3fV P=%+.3fV\r\n",
             (double)s_dpmzm_ctx.bias_i_v,
             (double)s_dpmzm_ctx.bias_q_v,
             (double)s_dpmzm_ctx.bias_p_v);
    printf("  dac code:      I=%u Q=%u P=%u\r\n",
           (unsigned)code_i,
           (unsigned)code_q,
           (unsigned)code_p);
    printf("  dac pin model: I=%.3fV Q=%.3fV P=%.3fV\r\n",
           (double)board_dac_code_to_dac_pin_voltage(code_i),
           (double)board_dac_code_to_dac_pin_voltage(code_q),
           (double)board_dac_code_to_dac_pin_voltage(code_p));
    printf("  note:          status is target/model only, no analog readback\r\n");
    printf("  adc dsp fs:    %u Hz\r\n",
           (unsigned)DSP_SAMPLE_RATE_HZ);
    printf("  pilot source:  %s\r\n",
           pilot_source_name(s_dpmzm_ctx.config->pilot_source));
    printf("  pilot output:  %s\r\n",
           pilot_output_mode);
    printf("  pilot timer:   %s, tim6_state=%lu\r\n",
           s_pilot_timer_running ? "running" : "stopped",
           (unsigned long)htim6.State);
    printf("  pilot tim6 fs: %.2f Hz\r\n",
           (double)pilot_timer_rate_hz);
    printf("  pilot actual:  irq=%.1f Hz sched=%.1f Hz, irq=%lu sched=%lu\r\n",
           (double)diag_irq_rate_hz,
           (double)diag_schedule_rate_hz,
           (unsigned long)diag_irq_count,
           (unsigned long)diag_schedule_count);
    printf("  pilot I:       %.1f Hz, %.1f mVpp\r\n",
           (double)s_dpmzm_ctx.config->pilot_i_freq_hz,
           (double)(s_dpmzm_ctx.config->pilot_i_amp_v * 2000.0f));
    printf("  pilot Q:       %.1f Hz, %.1f mVpp\r\n",
           (double)s_dpmzm_ctx.config->pilot_q_freq_hz,
           (double)(s_dpmzm_ctx.config->pilot_q_amp_v * 2000.0f));
    printf("  pilot dma:     %s, drops=%lu errors=%lu\r\n",
           s_pilot_dma_active ? "active" : "idle",
           (unsigned long)s_pilot_dma_drop_count,
           (unsigned long)s_pilot_dma_error_count);
    printf("  dump mode:     %s\r\n",
           dump_mode_name(s_dpmzm_ctx.config->dump_mode));
    printf("  scan blocks:   %lu\r\n",
           (unsigned long)s_dpmzm_ctx.config->scan_default_blocks);
}

static void print_debug(void)
{
    dpmzm_scan_trace_t scan_trace;
    const dpmzm_lock_context_t *lock_ctx = dpmzm_lock_get_context();
    uint32_t now_ms = HAL_GetTick();
    uint32_t tim6_irq_count;
    uint32_t dma_schedule_count;
    uint32_t dma_drop_count;
    uint32_t dma_error_count;
    bool pilot_dma_active;
    uint8_t pilot_dma_frame_count;
    uint8_t pilot_dma_frame_index;
    bool pilot_dma_contains_p;
    uint32_t scan_age_ms;
    uint32_t phase_age_ms;

    dpmzm_scan_trace_snapshot(&scan_trace);

    __disable_irq();
    tim6_irq_count = s_pilot_tim6_irq_count;
    dma_schedule_count = s_pilot_dma_schedule_count;
    dma_drop_count = s_pilot_dma_drop_count;
    dma_error_count = s_pilot_dma_error_count;
    pilot_dma_active = s_pilot_dma_active;
    pilot_dma_frame_count = s_pilot_dma_frame_count;
    pilot_dma_frame_index = s_pilot_dma_frame_index;
    pilot_dma_contains_p = s_pilot_dma_contains_p;
    __enable_irq();

    scan_age_ms = scan_trace.start_tick_ms == 0U ? 0U : now_ms - scan_trace.start_tick_ms;
    phase_age_ms = scan_trace.phase_tick_ms == 0U ? 0U : now_ms - scan_trace.phase_tick_ms;

    printf("[dpmzm] debug\r\n");
    printf("  tick:        %lu ms\r\n", (unsigned long)now_ms);
    printf("  scan:        running=%s gen=%lu phase=%s last_error=%s\r\n",
           scan_trace.running ? "yes" : "no",
           (unsigned long)scan_trace.generation,
           dpmzm_scan_trace_phase_name(scan_trace.phase),
           dpmzm_scan_trace_error_name(scan_trace.last_error));
    printf("  scan target: %s-%s sweep=%+.6fV index=%lu blocks=%lu/%lu sample=%lu/%lu\r\n",
           dpmzm_scan_stage_name(scan_trace.stage),
           dpmzm_scan_target_name(scan_trace.target),
           (double)scan_trace.sweep_v,
           (unsigned long)scan_trace.sweep_index,
           (unsigned long)scan_trace.block_index,
           (unsigned long)scan_trace.requested_blocks,
           (unsigned long)scan_trace.sample_index,
           (unsigned long)scan_trace.samples_per_block);
    printf("  scan bias:   I=%+.6fV Q=%+.6fV P=%+.6fV pilot=%s dump=%lu\r\n",
           (double)scan_trace.base_i_v,
           (double)scan_trace.base_q_v,
           (double)scan_trace.base_p_v,
           dpmzm_scan_pilot_mode_name(scan_trace.pilot_mode),
           (unsigned long)scan_trace.dump_mode);
    printf("  scan timing: age=%lu ms phase_age=%lu ms start=%lu phase_tick=%lu last=%lu\r\n",
           (unsigned long)scan_age_ms,
           (unsigned long)phase_age_ms,
           (unsigned long)scan_trace.start_tick_ms,
           (unsigned long)scan_trace.phase_tick_ms,
           (unsigned long)scan_trace.last_tick_ms);
    printf("  scan counts: ok=%lu fail=%lu bias=%lu drdy_to=%lu adc=%lu discard=%lu pilot_bias=%lu finalize=%lu overflow=%lu\r\n",
           (unsigned long)scan_trace.success_count,
           (unsigned long)scan_trace.failure_count,
           (unsigned long)scan_trace.bias_apply_fail_count,
           (unsigned long)scan_trace.adc_drdy_timeout_count,
           (unsigned long)scan_trace.adc_read_fail_count,
           (unsigned long)scan_trace.discard_fail_count,
           (unsigned long)scan_trace.pilot_bias_apply_fail_count,
           (unsigned long)scan_trace.finalize_fail_count,
           (unsigned long)scan_trace.point_overflow_fail_count);
    printf("  pilot:       timer=%s tim6_state=%lu dma=%s frame=%u/%u pframe=%s irq=%lu sched=%lu drops=%lu errors=%lu\r\n",
           s_pilot_timer_running ? "running" : "stopped",
           (unsigned long)htim6.State,
           pilot_dma_active ? "active" : "idle",
           (unsigned)pilot_dma_frame_index,
           (unsigned)pilot_dma_frame_count,
           pilot_dma_contains_p ? "yes" : "no",
           (unsigned long)tim6_irq_count,
           (unsigned long)dma_schedule_count,
           (unsigned long)dma_drop_count,
           (unsigned long)dma_error_count);
    printf("  hal:         spi_state=%lu spi_err=0x%08lx spi_sr=0x%08lx uart_g=%lu uart_rx=%lu uart_err=0x%08lx tim6_cr1=0x%08lx tim6_sr=0x%08lx tim6_cnt=%lu\r\n",
           (unsigned long)hspi1.State,
           (unsigned long)hspi1.ErrorCode,
           (unsigned long)hspi1.Instance->SR,
           (unsigned long)huart1.gState,
           (unsigned long)huart1.RxState,
           (unsigned long)huart1.ErrorCode,
           (unsigned long)htim6.Instance->CR1,
           (unsigned long)htim6.Instance->SR,
           (unsigned long)__HAL_TIM_GET_COUNTER(&htim6));
    printf("  lock:        state=%s enabled=%s busy=%s updates=%lu holds=%lu faults=%lu\r\n",
           lock_ctx == NULL ? "NULL" : dpmzm_lock_state_name(lock_ctx->state),
           (lock_ctx != NULL && lock_ctx->enabled) ? "yes" : "no",
           s_lock_cycle_busy ? "yes" : "no",
           lock_ctx == NULL ? 0UL : (unsigned long)lock_ctx->update_count,
           lock_ctx == NULL ? 0UL : (unsigned long)lock_ctx->hold_count,
           lock_ctx == NULL ? 0UL : (unsigned long)lock_ctx->fault_count);

    printf("DPMZMDEBUG,scan,%s,%lu,%s,%s,%s,%s,%.6f,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
           scan_trace.running ? "running" : "idle",
           (unsigned long)scan_trace.generation,
           dpmzm_scan_trace_phase_name(scan_trace.phase),
           dpmzm_scan_trace_error_name(scan_trace.last_error),
           dpmzm_scan_stage_name(scan_trace.stage),
           dpmzm_scan_target_name(scan_trace.target),
           (double)scan_trace.sweep_v,
           (unsigned long)scan_trace.sweep_index,
           (unsigned long)scan_trace.block_index,
           (unsigned long)scan_trace.sample_index,
           (unsigned long)scan_age_ms,
           (unsigned long)phase_age_ms,
           (unsigned long)scan_trace.success_count,
           (unsigned long)scan_trace.failure_count);
}

static void handle_set_pilot_open(const char *cmd)
{
    const char *arg = cmd + strlen("set pilot-open ");

    if (strcmp(arg, "on") == 0) {
        s_dpmzm_ctx.pilot_output_enabled = true;
        reset_pilot_phases();
        drive_next_sample_internal();
        if (!start_pilot_timer()) {
            s_dpmzm_ctx.pilot_output_enabled = false;
            reset_pilot_phases();
            (void)apply_biases();
            printf("[dpmzm] ERROR: pilot output failed to start\r\n");
            return;
        }
        printf("[dpmzm] pilot output -> continuous\r\n");
    } else if (strcmp(arg, "off") == 0) {
        s_dpmzm_ctx.pilot_output_enabled = false;
        stop_pilot_timer();
        reset_pilot_phases();
        (void)apply_biases();
        printf("[dpmzm] pilot output -> scan-only\r\n");
    } else {
        printf("[dpmzm] usage: set pilot-open on|off\r\n");
    }
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
        break;
    case 'q':
        break;
    case 'p':
        break;
    default:
        printf("[dpmzm] usage: set bias i|q|p <voltage>\r\n");
        return;
    }

    update_single_bias_state(path, value);
    if (timer_owns_dac_outputs()) {
        ret = 0;
    } else {
        ret = apply_biases();
    }
    printf("[dpmzm] bias %c -> %+.3f V (ret=%d)\r\n",
           path, (double)value, ret);
}

static void handle_set_pilot_src(const char *cmd)
{
    const char *arg = cmd + strlen("set pilot-src ");

    if (strcmp(arg, "onboard") == 0) {
        s_dpmzm_ctx.config->pilot_source = DPMZM_PILOT_SOURCE_ONBOARD;
        if (s_dpmzm_ctx.pilot_output_enabled) {
            stop_pilot_timer();
            (void)start_pilot_timer();
        }
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
        reset_pilot_phases();
        printf("[dpmzm] pilot I -> %.1f Hz, %.1f mVpp\r\n",
               (double)freq_hz, (double)mvpp);
        break;
    case 'q':
        s_dpmzm_ctx.config->pilot_q_freq_hz = freq_hz;
        s_dpmzm_ctx.config->pilot_q_amp_v = mvpp / 2000.0f;
        reset_pilot_phases();
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
    req.continuous_onboard_pilot = (req.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD);
    req.bias_apply_fn = app_dpmzm_scan_apply_bias_triplet;

    printf("[dpmzm] scan start: stage=%s target=%s start=%+.3f stop=%+.3f step=%+.3f blocks=%lu\r\n",
           dpmzm_scan_stage_name(req.stage),
           dpmzm_scan_target_name(req.target),
           (double)req.start_v,
           (double)req.stop_v,
           (double)req.step_v,
           blocks);

    if (!app_dpmzm_scan_begin(req.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        printf("[dpmzm] scan failed: onboard pilot start failed\r\n");
        return;
    }
    ok = dpmzm_scan_run(&req, &summary);
    app_dpmzm_scan_end();
    if (!ok) {
        printf("[dpmzm] scan failed\r\n");
        return;
    }

    printf("[dpmzm] scan done: best %s at %+.6f V (metric=%.9f)\r\n",
           summary.primary_metric_name,
           (double)summary.best_sweep_value,
           (double)summary.best_metric_value);
}

static void handle_auto_status(void)
{
    dpmzm_auto_print_status();
}

static void fill_auto_scan_template(dpmzm_scan_request_t *scan_template)
{
    if (scan_template == NULL) {
        return;
    }

    memset(scan_template, 0, sizeof(*scan_template));
    scan_template->blocks = s_dpmzm_ctx.config->scan_default_blocks;
    scan_template->settle_ms = 2U;
    scan_template->bias_i_v = s_dpmzm_ctx.bias_i_v;
    scan_template->bias_q_v = s_dpmzm_ctx.bias_q_v;
    scan_template->bias_p_v = s_dpmzm_ctx.bias_p_v;
    scan_template->bias_i_dac_channel = s_dpmzm_ctx.config->bias_i_dac_channel;
    scan_template->bias_q_dac_channel = s_dpmzm_ctx.config->bias_q_dac_channel;
    scan_template->bias_p_dac_channel = s_dpmzm_ctx.config->bias_p_dac_channel;
    scan_template->pilot_i_freq_hz = s_dpmzm_ctx.config->pilot_i_freq_hz;
    scan_template->pilot_q_freq_hz = s_dpmzm_ctx.config->pilot_q_freq_hz;
    scan_template->pilot_i_amp_v = s_dpmzm_ctx.config->pilot_i_amp_v;
    scan_template->pilot_q_amp_v = s_dpmzm_ctx.config->pilot_q_amp_v;
    scan_template->pilot_mode = to_scan_pilot_mode(s_dpmzm_ctx.config->pilot_source);
    scan_template->dump_mode = to_scan_dump_mode(s_dpmzm_ctx.config->dump_mode);
    scan_template->continuous_onboard_pilot =
        (scan_template->pilot_mode == DPMZM_SCAN_PILOT_ONBOARD);
    scan_template->bias_apply_fn = app_dpmzm_scan_apply_bias_triplet;
}

static void fill_lock_request(dpmzm_lock_request_t *req)
{
    if (req == NULL) {
        return;
    }

    memset(req, 0, sizeof(*req));
    fill_auto_scan_template(&req->scan_template);
    req->scan_template.blocks = DPMZM_LOCK_DEFAULT_IQ_BLOCKS;
    req->scan_template.settle_ms = DPMZM_LOCK_DEFAULT_SETTLE_MS;
    req->scan_template.dump_mode = DPMZM_SCAN_DUMP_METRICS;
    req->delta_v = DPMZM_LOCK_DEFAULT_DELTA_V;
    req->gain_v = DPMZM_LOCK_DEFAULT_GAIN_V;
    req->max_step_v = DPMZM_LOCK_DEFAULT_MAX_STEP_V;
    req->deadband_rel = DPMZM_LOCK_DEFAULT_DEADBAND;
    req->min_bias_v = -10.0f;
    req->max_bias_v = 10.0f;
    req->iq_anchor_window_v = DPMZM_LOCK_DEFAULT_IQ_WINDOW_V;
    req->p_anchor_window_v = DPMZM_LOCK_DEFAULT_P_WINDOW_V;
    req->iq_blocks = DPMZM_LOCK_DEFAULT_IQ_BLOCKS;
    req->p_blocks = DPMZM_LOCK_DEFAULT_P_BLOCKS;
    req->loop_interval_ms = DPMZM_LOCK_DEFAULT_INTERVAL_MS;
}

static void fill_lock_request_for_axis(dpmzm_lock_axis_t axis,
                                       dpmzm_lock_request_t *req)
{
    float scale = 1.0f;

    fill_lock_request(req);
    if (req == NULL || axis >= DPMZM_LOCK_AXIS_COUNT) {
        return;
    }

    scale = s_lock_adaptive[axis].scale;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    req->gain_v = lock_axis_base_gain(axis) * scale;
    req->max_step_v = fminf(lock_axis_base_max_step(axis) * scale,
                            DPMZM_LOCK_ADAPTIVE_HARD_MAX_STEP_V);
    req->iq_anchor_window_v = lock_axis_dynamic_window(axis);
}

static int apply_lock_step_result(const dpmzm_lock_step_result_t *result)
{
    float vi = s_dpmzm_ctx.bias_i_v;
    float vq = s_dpmzm_ctx.bias_q_v;
    float vp = s_dpmzm_ctx.bias_p_v;

    if (result == NULL || !result->probe.valid) {
        return -1;
    }

    switch (result->probe.axis) {
    case DPMZM_LOCK_AXIS_I:
        vi = result->new_bias_v;
        break;
    case DPMZM_LOCK_AXIS_Q:
        vq = result->new_bias_v;
        break;
    case DPMZM_LOCK_AXIS_P:
        vp = result->new_bias_v;
        break;
    default:
        return -1;
    }

    return app_dpmzm_scan_apply_bias_triplet(vi, vq, vp);
}

static bool measure_lock_axis_point(dpmzm_lock_axis_t axis,
                                    float sweep_v,
                                    uint32_t blocks,
                                    dpmzm_lock_local_measurement_t *out)
{
    dpmzm_scan_request_t req;
    dpmzm_measurement_t m;
    float metric;

    if (out == NULL || axis >= DPMZM_LOCK_AXIS_COUNT) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    fill_auto_scan_template(&req);
    req.dump_mode = DPMZM_SCAN_DUMP_METRICS;
    req.blocks = blocks;
    req.settle_ms = DPMZM_LOCK_DEFAULT_SETTLE_MS;
    lock_set_stage_target(&req, axis);

    if (!dpmzm_scan_measure_point(&req, sweep_v, &m)) {
        return false;
    }

    metric = lock_metric_for_axis(axis, &m);
    out->valid = true;
    out->sweep_v = sweep_v;
    out->metric = metric;
    out->objective = app_objective_from_metric(metric);
    out->dbm = app_metric_to_dbm(metric);
    out->dc_v = m.dc_mean;
    return true;
}

static bool run_lock_local_recheck(dpmzm_lock_axis_t axis,
                                   float center_v,
                                   float window_v,
                                   float step_v,
                                   bool prefer_dc_limit,
                                   float dc_limit_abs,
                                   dpmzm_lock_local_measurement_t *best_out)
{
    dpmzm_lock_request_t req;
    dpmzm_lock_local_measurement_t best;
    dpmzm_lock_local_measurement_t best_metric_any;
    float min_v = -10.0f;
    float max_v = 10.0f;
    float start_v;
    float stop_v;
    float value;
    uint32_t blocks;
    bool any = false;
    bool any_dc_valid = false;

    if (best_out == NULL || axis >= DPMZM_LOCK_AXIS_COUNT || step_v <= 0.0f) {
        return false;
    }

    memset(&best, 0, sizeof(best));
    memset(&best_metric_any, 0, sizeof(best_metric_any));
    fill_lock_request_for_axis(axis, &req);
    blocks = (axis == DPMZM_LOCK_AXIS_P) ? req.p_blocks : req.iq_blocks;

    start_v = app_clampf(center_v - fabsf(window_v), min_v, max_v);
    stop_v = app_clampf(center_v + fabsf(window_v), min_v, max_v);

    if (!app_dpmzm_scan_begin(req.scan_template.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        return false;
    }

    for (value = start_v; value <= stop_v + step_v * 0.5f; value += step_v) {
        dpmzm_lock_local_measurement_t current;
        float sweep_v = app_clampf(value, start_v, stop_v);

        if (!measure_lock_axis_point(axis, sweep_v, blocks, &current)) {
            app_dpmzm_scan_end();
            return false;
        }

        if (!any || current.objective < best_metric_any.objective) {
            best_metric_any = current;
        }
        any = true;

        if (prefer_dc_limit && fabsf(current.dc_v) > dc_limit_abs) {
            continue;
        }
        if (!any_dc_valid || current.objective < best.objective) {
            best = current;
            any_dc_valid = true;
        }
    }

    app_dpmzm_scan_end();
    if (!any) {
        return false;
    }

    *best_out = any_dc_valid ? best : best_metric_any;
    return true;
}

static bool run_lock_probe_once(dpmzm_lock_axis_t axis,
                                dpmzm_lock_probe_result_t *result_out)
{
    dpmzm_lock_request_t req;
    dpmzm_lock_probe_result_t result;
    bool ok;

    memset(&result, 0, sizeof(result));
    fill_lock_request_for_axis(axis, &req);

    if (!app_dpmzm_scan_begin(req.scan_template.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        printf("[dpmzm][lock] probe failed: onboard pilot start failed\r\n");
        result.axis = axis;
        result.error_code = DPMZM_LOCK_ERR_MEASURE;
        if (result_out != NULL) {
            *result_out = result;
        }
        return false;
    }
    ok = dpmzm_lock_probe(&req, axis, &result);
    app_dpmzm_scan_end();

    if (result_out != NULL) {
        *result_out = result;
    }
    if (!ok) {
        dpmzm_lock_record_fault(result.error_code);
    }
    return ok;
}

static void maybe_run_boundary_recheck(const dpmzm_lock_step_result_t *result)
{
    const dpmzm_lock_context_t *ctx;
    dpmzm_lock_boundary_axis_t *state;
    dpmzm_lock_local_measurement_t selected;
    dpmzm_lock_axis_t axis;
    float current_obj;
    float degrade_db;
    float old_anchor_v;
    float old_window_v;
    float shift_v;
    float new_window_v;
    uint32_t step_index;

    if (result == NULL || !result->probe.valid) {
        return;
    }

    axis = result->probe.axis;
    if (axis != DPMZM_LOCK_AXIS_I && axis != DPMZM_LOCK_AXIS_Q) {
        return;
    }

    state = &s_lock_boundary[axis];
    if (state->window_v <= 0.0f) {
        state->window_v = DPMZM_LOCK_DEFAULT_IQ_WINDOW_V;
    }
    if (state->cooldown_steps > 0) {
        state->cooldown_steps--;
    }

    current_obj = app_objective_from_metric(result->probe.metric_center);
    if (!state->valid || current_obj < state->best_objective) {
        state->valid = true;
        state->best_objective = current_obj;
        state->best_dbm = result->probe.metric_center_dbm;
        state->best_bias_v = result->probe.center_v;
    }

    if (result->clamped) {
        state->clamp_streak++;
        state->stable_steps = 0U;
    } else {
        state->clamp_streak = 0U;
        if (state->window_v > DPMZM_LOCK_DEFAULT_IQ_WINDOW_V) {
            state->stable_steps++;
            if (state->stable_steps >= DPMZM_LOCK_BOUNDARY_SHRINK_STABLE) {
                float old = state->window_v;
                state->window_v = fmaxf(DPMZM_LOCK_DEFAULT_IQ_WINDOW_V,
                                        state->window_v * DPMZM_LOCK_BOUNDARY_SHRINK);
                state->stable_steps = 0U;
                if (state->window_v < old - 1.0e-6f) {
                    printf("[dpmzm][lock] boundary window shrink axis=%s %.4fV -> %.4fV\r\n",
                           dpmzm_lock_axis_name(axis),
                           (double)old,
                           (double)state->window_v);
                }
            }
        }
    }

    degrade_db = result->probe.metric_center_dbm - state->best_dbm;
    if (state->cooldown_steps > 0 ||
        state->clamp_streak < DPMZM_LOCK_BOUNDARY_COUNT ||
        degrade_db < DPMZM_LOCK_BOUNDARY_DEGRADE_DB) {
        return;
    }

    ctx = dpmzm_lock_get_context();
    old_anchor_v = lock_axis_anchor_bias(ctx, axis);
    old_window_v = state->window_v;
    step_index = ctx == NULL ? 0U : (ctx->update_count + ctx->hold_count);

    printf("[dpmzm][lock] boundary recheck start axis=%s center=%+.4fV degrade=%.2fdB window=+/-%.3fV\r\n",
           dpmzm_lock_axis_name(axis),
           (double)lock_axis_current_bias(axis),
           (double)degrade_db,
           (double)DPMZM_LOCK_BOUNDARY_WINDOW_V);

    if (!run_lock_local_recheck(axis,
                                lock_axis_current_bias(axis),
                                DPMZM_LOCK_BOUNDARY_WINDOW_V,
                                DPMZM_LOCK_BOUNDARY_STEP_V,
                                false,
                                0.0f,
                                &selected)) {
        printf("[dpmzm][lock] boundary recheck failed axis=%s\r\n",
               dpmzm_lock_axis_name(axis));
        state->cooldown_steps = DPMZM_LOCK_BOUNDARY_COOLDOWN_STEPS;
        return;
    }

    (void)apply_lock_axis_bias(axis, selected.sweep_v);
    dpmzm_lock_update_axis_anchor(axis, selected.sweep_v);

    shift_v = fabsf(selected.sweep_v - old_anchor_v);
    new_window_v = fmaxf(DPMZM_LOCK_DEFAULT_IQ_WINDOW_V,
                         shift_v + DPMZM_LOCK_BOUNDARY_EXPAND_MARGIN_V);
    if (state->last_recheck_step != 0U &&
        step_index >= state->last_recheck_step &&
        (step_index - state->last_recheck_step) <= DPMZM_LOCK_BOUNDARY_FAST_REPEAT) {
        new_window_v = fmaxf(new_window_v,
                             old_window_v * DPMZM_LOCK_BOUNDARY_FAST_EXPAND);
    }
    state->window_v = fminf(new_window_v, DPMZM_LOCK_BOUNDARY_MAX_WINDOW_V);
    state->valid = true;
    state->best_objective = selected.objective;
    state->best_dbm = selected.dbm;
    state->best_bias_v = selected.sweep_v;
    state->clamp_streak = 0U;
    state->stable_steps = 0U;
    state->cooldown_steps = DPMZM_LOCK_BOUNDARY_COOLDOWN_STEPS;
    state->last_recheck_step = step_index;

    printf("[dpmzm][lock] boundary recheck axis=%s anchor %+.4fV -> %+.4fV metric=%.2fdBm dc=%+.6fV window %.4fV -> %.4fV\r\n",
           dpmzm_lock_axis_name(axis),
           (double)old_anchor_v,
           (double)selected.sweep_v,
           (double)selected.dbm,
           (double)selected.dc_v,
           (double)old_window_v,
           (double)state->window_v);
}

static float update_dc_guard_average(float rise_db)
{
    uint8_t window = DPMZM_LOCK_DC_GUARD_AVG_WINDOW;
    float sum = 0.0f;

    if (window == 0U) {
        return rise_db;
    }

    s_lock_dc_guard.rise_history_db[s_lock_dc_guard.history_index % window] = rise_db;
    s_lock_dc_guard.history_index = (uint8_t)((s_lock_dc_guard.history_index + 1U) % window);
    if (s_lock_dc_guard.history_count < window) {
        s_lock_dc_guard.history_count++;
    }

    for (uint8_t i = 0U; i < s_lock_dc_guard.history_count; i++) {
        sum += s_lock_dc_guard.rise_history_db[i];
    }
    return sum / (float)s_lock_dc_guard.history_count;
}

static bool dc_guard_should_trigger(float dc_now_v,
                                    float *rise_db_out,
                                    float *avg_db_out)
{
    float ref;
    float ratio;
    float rise_db;
    float avg_db;

    if (!s_lock_dc_guard.anchor_valid) {
        s_lock_dc_guard.anchor_valid = true;
        s_lock_dc_guard.anchor_dc_v = dc_now_v;
    }

    ref = fmaxf(fabsf(s_lock_dc_guard.anchor_dc_v), DPMZM_LOCK_DC_GUARD_FLOOR_V);
    ratio = fabsf(dc_now_v) / ref;
    rise_db = 20.0f * log10f(fmaxf(ratio, 1.0e-12f));
    avg_db = update_dc_guard_average(rise_db);

    if (rise_db_out != NULL) {
        *rise_db_out = rise_db;
    }
    if (avg_db_out != NULL) {
        *avg_db_out = avg_db;
    }

    if (s_lock_dc_guard.cooldown_steps > 0) {
        s_lock_dc_guard.cooldown_steps--;
        return false;
    }

    if (avg_db <= DPMZM_LOCK_DC_GUARD_RELEASE_DB) {
        s_lock_dc_guard.armed = false;
        s_lock_dc_guard.trigger_count = 0U;
        return false;
    }

    if (avg_db <= DPMZM_LOCK_DC_GUARD_THRESHOLD_DB && !s_lock_dc_guard.armed) {
        s_lock_dc_guard.trigger_count = 0U;
        return false;
    }

    if (avg_db > DPMZM_LOCK_DC_GUARD_THRESHOLD_DB) {
        s_lock_dc_guard.armed = true;
    } else if (s_lock_dc_guard.armed) {
        return false;
    }

    s_lock_dc_guard.trigger_count++;
    if (s_lock_dc_guard.trigger_count >= DPMZM_LOCK_DC_GUARD_COUNT) {
        s_lock_dc_guard.trigger_count = 0U;
        s_lock_dc_guard.cooldown_steps = DPMZM_LOCK_DC_GUARD_COOLDOWN_STEPS;
        return true;
    }
    return false;
}

static void maybe_run_dc_guard_recheck(const dpmzm_lock_step_result_t *result)
{
    static const dpmzm_lock_axis_t recheck_sequence[] = {
        DPMZM_LOCK_AXIS_I,
        DPMZM_LOCK_AXIS_Q,
        DPMZM_LOCK_AXIS_P
    };
    float rise_db = 0.0f;
    float avg_db = 0.0f;
    float dc_limit_abs;
    float best_dc_abs;
    bool have_best_dc = false;

    if (result == NULL || !result->probe.valid) {
        return;
    }

    if (!dc_guard_should_trigger(result->probe.dc_center_v, &rise_db, &avg_db)) {
        return;
    }

    dc_limit_abs = fmaxf(fabsf(s_lock_dc_guard.anchor_dc_v),
                         DPMZM_LOCK_DC_GUARD_FLOOR_V) *
                   powf(10.0f, DPMZM_LOCK_DC_GUARD_THRESHOLD_DB / 20.0f);
    best_dc_abs = fabsf(s_lock_dc_guard.anchor_dc_v);

    printf("[dpmzm][lock] dc guard recheck start: dc=%+.6fV rise=%.2fdB avg=%.2fdB limit_abs=%.6fV\r\n",
           (double)result->probe.dc_center_v,
           (double)rise_db,
           (double)avg_db,
           (double)dc_limit_abs);

    for (uint32_t i = 0U; i < (uint32_t)(sizeof(recheck_sequence) / sizeof(recheck_sequence[0])); i++) {
        dpmzm_lock_axis_t axis = recheck_sequence[i];
        dpmzm_lock_local_measurement_t selected;
        float window = (axis == DPMZM_LOCK_AXIS_P) ?
                       DPMZM_LOCK_DC_RECHECK_P_WINDOW_V :
                       DPMZM_LOCK_DC_RECHECK_IQ_WINDOW_V;

        if (!run_lock_local_recheck(axis,
                                    lock_axis_current_bias(axis),
                                    window,
                                    DPMZM_LOCK_DC_RECHECK_STEP_V,
                                    true,
                                    dc_limit_abs,
                                    &selected)) {
            printf("[dpmzm][lock] dc guard recheck axis=%s failed\r\n",
                   dpmzm_lock_axis_name(axis));
            continue;
        }

        (void)apply_lock_axis_bias(axis, selected.sweep_v);
        if (!have_best_dc || fabsf(selected.dc_v) < best_dc_abs) {
            best_dc_abs = fabsf(selected.dc_v);
            have_best_dc = true;
        }
        printf("[dpmzm][lock] dc guard recheck axis=%s select=%+.4fV metric=%.2fdBm dc=%+.6fV\r\n",
               dpmzm_lock_axis_name(axis),
               (double)selected.sweep_v,
               (double)selected.dbm,
               (double)selected.dc_v);
    }

    if (have_best_dc && best_dc_abs <= fmaxf(fabsf(s_lock_dc_guard.anchor_dc_v),
                                            DPMZM_LOCK_DC_GUARD_FLOOR_V)) {
        s_lock_dc_guard.anchor_dc_v = best_dc_abs;
    }
}

static bool run_lock_step_once(dpmzm_lock_axis_t axis,
                               bool print_result,
                               dpmzm_lock_step_result_t *result_out)
{
    dpmzm_lock_request_t req;
    dpmzm_lock_step_result_t result;
    bool ok;
    int apply_ret = 0;

    memset(&result, 0, sizeof(result));
    fill_lock_request_for_axis(axis, &req);

    if (!app_dpmzm_scan_begin(req.scan_template.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        printf("[dpmzm][lock] step failed: onboard pilot start failed\r\n");
        result.probe.axis = axis;
        result.error_code = DPMZM_LOCK_ERR_MEASURE;
        if (result_out != NULL) {
            *result_out = result;
        }
        dpmzm_lock_record_fault(DPMZM_LOCK_ERR_MEASURE);
        return false;
    }
    ok = dpmzm_lock_step(&req, axis, &result);
    app_dpmzm_scan_end();

    if (ok) {
        apply_ret = apply_lock_step_result(&result);
        if (apply_ret != 0) {
            ok = false;
            result.error_code = DPMZM_LOCK_ERR_MEASURE;
        }
    }

    if (ok) {
        update_lock_adaptive_state(axis,
                                   result.applied_step_v,
                                   result.held_by_deadband,
                                   result.clamped);
    }

    if (print_result) {
        dpmzm_lock_print_step_result(&result);
        if (ok) {
            printf("[dpmzm][lock] applied step ret=%d\r\n", apply_ret);
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    }

    if (ok) {
        dpmzm_lock_record_step(&result);
        maybe_run_boundary_recheck(&result);
        maybe_run_dc_guard_recheck(&result);
    } else {
        dpmzm_lock_record_fault(result.error_code);
    }
    return ok;
}

static void handle_auto_coarse(void)
{
    dpmzm_auto_coarse_request_t req;
    dpmzm_auto_coarse_result_t result;
    bool ok;
    int apply_ret = 0;

    memset(&req, 0, sizeof(req));
    memset(&result, 0, sizeof(result));

    req.sweep_min_v = -9.0f;
    req.sweep_max_v = 9.0f;
    req.sweep_step_v = 0.5f;
    fill_auto_scan_template(&req.scan_template);
    req.iq_blocks = 4U;
    req.p_blocks = 10U;

    printf("[dpmzm][auto] coarse start: range=%+.1f..%+.1fV step=%.3f blocks IQ=%lu P=%lu\r\n",
           (double)req.sweep_min_v,
           (double)req.sweep_max_v,
           (double)req.sweep_step_v,
           (unsigned long)req.iq_blocks,
           (unsigned long)req.p_blocks);

    if (!app_dpmzm_scan_begin(req.scan_template.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        printf("[dpmzm][auto] failed: onboard pilot start failed\r\n");
        return;
    }

    ok = dpmzm_auto_run_coarse(&req, &result);
    app_dpmzm_scan_end();

    dpmzm_auto_print_result(&result);
    if (!ok) {
        printf("[dpmzm][auto] coarse failed\r\n");
        return;
    }

    apply_ret = app_dpmzm_scan_apply_bias_triplet(result.i_mitp_coarse_v,
                                                  result.q_mitp_coarse_v,
                                                  result.p_qtp_coarse_v);
    printf("[dpmzm][auto] applied coarse result: I=%+.3fV Q=%+.3fV P=%+.3fV (ret=%d)\r\n",
           (double)result.i_mitp_coarse_v,
           (double)result.q_mitp_coarse_v,
           (double)result.p_qtp_coarse_v,
           apply_ret);
}

static void handle_auto_fine(void)
{
    const dpmzm_auto_context_t *auto_ctx = dpmzm_auto_get_context();
    dpmzm_auto_fine_request_t req;
    dpmzm_auto_fine_result_t result;
    bool ok;
    int apply_ret = 0;

    memset(&req, 0, sizeof(req));
    memset(&result, 0, sizeof(result));

    if (auto_ctx == NULL || !auto_ctx->has_result ||
        auto_ctx->last_result.error != DPMZM_AUTO_OK ||
        !auto_ctx->last_result.p_qtp_valid ||
        !auto_ctx->last_result.i_mitp_valid ||
        !auto_ctx->last_result.q_mitp_valid) {
        printf("[dpmzm][auto] fine refused: run 'dpmzm auto coarse' first\r\n");
        return;
    }

    fill_auto_scan_template(&req.scan_template);
    req.coarse_result = auto_ctx->last_result;
    req.sweep_min_v = -9.0f;
    req.sweep_max_v = 9.0f;
    req.wide_range_v = 2.0f;
    req.wide_step_v = 0.2f;
    req.p_fine_range_v = 0.6f;
    req.iq_fine_range_v = 0.6f;
    req.fine_step_v = 0.01f;
    req.iq_blocks = 4U;
    req.p_blocks = 10U;

    printf("[dpmzm][auto] fine start: P/I/Q/P turn search + prelock I/Q recheck step=%.3f blocks IQ=%lu P=%lu\r\n",
           (double)req.fine_step_v,
           (unsigned long)req.iq_blocks,
           (unsigned long)req.p_blocks);

    if (!app_dpmzm_scan_begin(req.scan_template.pilot_mode == DPMZM_SCAN_PILOT_ONBOARD)) {
        printf("[dpmzm][auto] fine failed: onboard pilot start failed\r\n");
        return;
    }

    ok = dpmzm_auto_run_fine(&req, &result);
    app_dpmzm_scan_end();

    dpmzm_auto_print_fine_result(&result);
    if (!ok) {
        printf("[dpmzm][auto] fine failed\r\n");
        return;
    }

    apply_ret = app_dpmzm_scan_apply_bias_triplet(result.i_mitp_fine_v,
                                                  result.q_mitp_fine_v,
                                                  result.p_qtp_fine_v);
    printf("[dpmzm][auto] applied fine result: I=%+.3fV Q=%+.3fV P=%+.3fV (ret=%d)\r\n",
           (double)result.i_mitp_fine_v,
           (double)result.q_mitp_fine_v,
           (double)result.p_qtp_fine_v,
           apply_ret);
}

static void handle_lock_start(void);

static bool parse_optional_auto_lock_biases(const char *cmd,
                                            float *i_out,
                                            float *q_out,
                                            float *p_out)
{
    const char *args = NULL;
    char *endptr = NULL;
    float values[3] = {0.0f, 0.0f, 0.0f};

    if (i_out == NULL || q_out == NULL || p_out == NULL) {
        return false;
    }

    if (strncmp(cmd, "auto lock", 9) == 0) {
        args = cmd + strlen("auto lock");
    } else if (strncmp(cmd, "auto run", 8) == 0) {
        args = cmd + strlen("auto run");
    } else {
        return false;
    }

    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        *i_out = 0.0f;
        *q_out = 0.0f;
        *p_out = 0.0f;
        return true;
    }

    for (uint32_t idx = 0U; idx < 3U; idx++) {
        values[idx] = strtof(args, &endptr);
        if (endptr == args) {
            return false;
        }
        args = endptr;
        while (*args == ' ') {
            args++;
        }
    }
    if (*args != '\0') {
        return false;
    }

    *i_out = app_clampf(values[0], -10.0f, 10.0f);
    *q_out = app_clampf(values[1], -10.0f, 10.0f);
    *p_out = app_clampf(values[2], -10.0f, 10.0f);
    return true;
}

static void handle_auto_lock(const char *cmd)
{
    const dpmzm_auto_context_t *auto_ctx;
    float init_i = 0.0f;
    float init_q = 0.0f;
    float init_p = 0.0f;

    if (!parse_optional_auto_lock_biases(cmd, &init_i, &init_q, &init_p)) {
        printf("[dpmzm][auto] usage: auto lock [initial_i initial_q initial_p]\r\n");
        printf("[dpmzm][auto] alias: auto run [initial_i initial_q initial_p]\r\n");
        return;
    }

    printf("[dpmzm][auto] full auto-lock start: initial I=%+.3fV Q=%+.3fV P=%+.3fV\r\n",
           (double)init_i,
           (double)init_q,
           (double)init_p);

    dpmzm_lock_stop();
    reset_lock_runtime_state();
    handle_set_dump("set dump metrics");
    if (s_dpmzm_ctx.config->pilot_source == DPMZM_PILOT_SOURCE_ONBOARD &&
        !s_dpmzm_ctx.pilot_output_enabled) {
        handle_set_pilot_open("set pilot-open on");
        if (!s_dpmzm_ctx.pilot_output_enabled) {
            printf("[dpmzm][auto] full auto-lock refused: pilot output failed\r\n");
            return;
        }
    }

    if (app_dpmzm_scan_apply_bias_triplet(init_i, init_q, init_p) != 0) {
        printf("[dpmzm][auto] full auto-lock refused: initial bias apply failed\r\n");
        return;
    }

    handle_auto_coarse();
    auto_ctx = dpmzm_auto_get_context();
    if (auto_ctx == NULL || !auto_ctx->has_result ||
        auto_ctx->last_result.error != DPMZM_AUTO_OK ||
        !auto_ctx->last_result.p_qtp_valid ||
        !auto_ctx->last_result.i_mitp_valid ||
        !auto_ctx->last_result.q_mitp_valid) {
        printf("[dpmzm][auto] full auto-lock stopped: coarse failed\r\n");
        return;
    }

    handle_auto_fine();
    auto_ctx = dpmzm_auto_get_context();
    if (auto_ctx == NULL || !auto_ctx->has_fine_result ||
        auto_ctx->last_fine_result.error != DPMZM_AUTO_OK ||
        !auto_ctx->last_fine_result.p_qtp_valid ||
        !auto_ctx->last_fine_result.i_mitp_valid ||
        !auto_ctx->last_fine_result.q_mitp_valid) {
        printf("[dpmzm][auto] full auto-lock stopped: fine failed\r\n");
        return;
    }

    handle_lock_start();
    printf("[dpmzm][auto] full auto-lock done: lock running after auto fine\r\n");
}

static void handle_lock_probe(const char *cmd)
{
    dpmzm_lock_axis_t axis;
    dpmzm_lock_probe_result_t result;
    const char *arg = cmd + strlen("lock probe ");

    if (!dpmzm_lock_axis_from_char(arg[0], &axis) || arg[1] != '\0') {
        printf("[dpmzm][lock] usage: lock probe i|q|p\r\n");
        return;
    }

    if (run_lock_probe_once(axis, &result)) {
        dpmzm_lock_print_probe_result(&result);
    } else {
        dpmzm_lock_print_probe_result(&result);
        printf("[dpmzm][lock] probe failed\r\n");
    }
}

static void handle_lock_step(const char *cmd)
{
    dpmzm_lock_axis_t axis;
    dpmzm_lock_step_result_t result;
    const char *arg = cmd + strlen("lock step ");

    if (!dpmzm_lock_axis_from_char(arg[0], &axis) || arg[1] != '\0') {
        printf("[dpmzm][lock] usage: lock step i|q|p\r\n");
        return;
    }

    if (!run_lock_step_once(axis, true, &result)) {
        printf("[dpmzm][lock] step failed\r\n");
    }
}

static void handle_lock_start(void)
{
    if (s_dpmzm_ctx.config->pilot_source == DPMZM_PILOT_SOURCE_ONBOARD &&
        !s_dpmzm_ctx.pilot_output_enabled) {
        handle_set_pilot_open("set pilot-open on");
        if (!s_dpmzm_ctx.pilot_output_enabled) {
            printf("[dpmzm][lock] start refused: pilot output failed\r\n");
            return;
        }
    }

    dpmzm_lock_start_with_anchor(s_dpmzm_ctx.bias_i_v,
                                 s_dpmzm_ctx.bias_q_v,
                                 s_dpmzm_ctx.bias_p_v);
    reset_lock_runtime_state();
    s_lock_last_cycle_ms = 0U;
    printf("[dpmzm][lock] start: sequence P-I-Q adaptive gradient, anchor I=%+.3fV Q=%+.3fV P=%+.3fV\r\n",
           (double)s_dpmzm_ctx.bias_i_v,
           (double)s_dpmzm_ctx.bias_q_v,
           (double)s_dpmzm_ctx.bias_p_v);
}

static void handle_lock_stop(void)
{
    dpmzm_lock_stop();
    printf("[dpmzm][lock] stop\r\n");
}

static void handle_lock_status(void)
{
    dpmzm_lock_request_t req;

    fill_lock_request(&req);
    dpmzm_lock_print_status();
    printf("  delta:      %.3fV\r\n", (double)req.delta_v);
    printf("  gain:       %.3fV\r\n", (double)req.gain_v);
    printf("  max step:   %.3fV\r\n", (double)req.max_step_v);
    printf("  deadband:   %.3f\r\n", (double)req.deadband_rel);
    printf("  blocks:     IQ=%lu P=%lu\r\n",
           (unsigned long)req.iq_blocks,
           (unsigned long)req.p_blocks);
    printf("  window:     IQ=+/-%.3fV P=+/-%.3fV\r\n",
           (double)req.iq_anchor_window_v,
           (double)req.p_anchor_window_v);
    printf("  adaptive:   scale I=%.2f Q=%.2f P=%.2f hard_max=%.3fV\r\n",
           (double)s_lock_adaptive[DPMZM_LOCK_AXIS_I].scale,
           (double)s_lock_adaptive[DPMZM_LOCK_AXIS_Q].scale,
           (double)s_lock_adaptive[DPMZM_LOCK_AXIS_P].scale,
           (double)DPMZM_LOCK_ADAPTIVE_HARD_MAX_STEP_V);
    printf("  boundary:   win I=%.3fV Q=%.3fV degrade=%.1fdB count=%u\r\n",
           (double)s_lock_boundary[DPMZM_LOCK_AXIS_I].window_v,
           (double)s_lock_boundary[DPMZM_LOCK_AXIS_Q].window_v,
           (double)DPMZM_LOCK_BOUNDARY_DEGRADE_DB,
           (unsigned)DPMZM_LOCK_BOUNDARY_COUNT);
    printf("  dc guard:   anchor=%s %.6fV threshold=%.1fdB release=%.1fdB count=%u\r\n",
           s_lock_dc_guard.anchor_valid ? "yes" : "no",
           (double)s_lock_dc_guard.anchor_dc_v,
           (double)DPMZM_LOCK_DC_GUARD_THRESHOLD_DB,
           (double)DPMZM_LOCK_DC_GUARD_RELEASE_DB,
           (unsigned)DPMZM_LOCK_DC_GUARD_COUNT);
    printf("  settle:     %lu ms\r\n", (unsigned long)req.scan_template.settle_ms);
    printf("  interval:   %lu ms\r\n", (unsigned long)req.loop_interval_ms);
}

static bool wait_and_read_capture_sample(ads131m02_sample_t *sample_out)
{
    uint32_t t0;

    if (sample_out == NULL) {
        return false;
    }

    t0 = HAL_GetTick();
    while (board_adc_drdy_read() != 0U) {
        if ((HAL_GetTick() - t0) > DPMZM_CAPTURE_DRDY_TIMEOUT_MS) {
            return false;
        }
    }

    if (ads131m02_read_sample(sample_out) != 0 || !sample_out->valid) {
        return false;
    }

    return true;
}

static void print_capture_raw_line(uint32_t sample_index,
                                   float bias_i_v,
                                   float bias_q_v,
                                   float bias_p_v,
                                   const char *pilot_output_mode,
                                   const dpmzm_capture_sample_t *sample)
{
    float ch0_v;
    float ch1_v;

    if (sample == NULL) {
        return;
    }

    ch0_v = ads131m02_code_to_voltage(sample->ch0_code, ADS131M02_GAIN_1);
    ch1_v = ads131m02_code_to_voltage(sample->ch1_code, ADS131M02_GAIN_1);

    printf("DPMZMCAPTURE,raw,%lu,%ld,%ld,%.9f,%.9f,%u,%.6f,%.6f,%.6f,%s,%s\r\n",
           (unsigned long)sample_index,
           (long)sample->ch0_code,
           (long)sample->ch1_code,
           (double)ch0_v,
           (double)ch1_v,
           (unsigned)DSP_SAMPLE_RATE_HZ,
           (double)bias_i_v,
           (double)bias_q_v,
           (double)bias_p_v,
           pilot_source_name(s_dpmzm_ctx.config->pilot_source),
           pilot_output_mode);
}

static bool capture_begin_onboard_pilot(bool *temporary_pilot_started)
{
    if (temporary_pilot_started == NULL) {
        return false;
    }

    *temporary_pilot_started = false;

    if (s_dpmzm_ctx.config->pilot_source != DPMZM_PILOT_SOURCE_ONBOARD) {
        return true;
    }

    if (s_dpmzm_ctx.pilot_output_enabled) {
        if (!s_pilot_timer_running) {
            reset_pilot_phases();
            (void)apply_current_drive();
            if (!start_pilot_timer()) {
                (void)apply_biases();
                return false;
            }
        }
        return true;
    }

    stop_pilot_timer();
    s_capture_pilot_active = true;
    reset_pilot_phases();
    (void)apply_current_drive();
    if (!start_pilot_timer()) {
        s_capture_pilot_active = false;
        (void)apply_biases();
        return false;
    }

    *temporary_pilot_started = true;
    return true;
}

static void capture_end_onboard_pilot(bool temporary_pilot_started)
{
    if (!temporary_pilot_started) {
        return;
    }

    stop_pilot_timer();
    s_capture_pilot_active = false;
    reset_pilot_phases();
    (void)apply_biases();
}

static void handle_capture_raw(const char *cmd)
{
    const char *args = cmd + strlen("capture raw ");
    char *endptr = NULL;
    unsigned long requested_samples = 0UL;
    unsigned long settle_ms = DPMZM_CAPTURE_SETTLE_DEFAULT_MS;
    bool temporary_pilot_started = false;
    const char *pilot_output_mode = "scan-only";
    float bias_i_v;
    float bias_q_v;
    float bias_p_v;
    unsigned long i;

    if (*args == '\0') {
        printf("[dpmzm] usage: capture raw <samples> [settle_ms]\r\n");
        return;
    }

    requested_samples = strtoul(args, &endptr, 10);
    if (endptr == args || requested_samples == 0UL ||
        requested_samples > DPMZM_CAPTURE_SAMPLES_MAX) {
        printf("[dpmzm] samples must be in range 1..%u\r\n",
               (unsigned)DPMZM_CAPTURE_SAMPLES_MAX);
        return;
    }

    while (*endptr == ' ') {
        endptr++;
    }

    if (*endptr != '\0') {
        settle_ms = strtoul(endptr, &endptr, 10);
        if (settle_ms == 0UL) {
            printf("[dpmzm] settle_ms must be a positive integer\r\n");
            return;
        }
        while (*endptr == ' ') {
            endptr++;
        }
        if (*endptr != '\0') {
            printf("[dpmzm] usage: capture raw <samples> [settle_ms]\r\n");
            return;
        }
    }

    bias_i_v = s_dpmzm_ctx.bias_i_v;
    bias_q_v = s_dpmzm_ctx.bias_q_v;
    bias_p_v = s_dpmzm_ctx.bias_p_v;

    if (!timer_owns_dac_outputs()) {
        if (apply_biases() != 0) {
            printf("[dpmzm] capture failed: could not apply current biases\r\n");
            return;
        }
    }

    if (!capture_begin_onboard_pilot(&temporary_pilot_started)) {
        printf("[dpmzm] capture failed: onboard pilot start failed\r\n");
        return;
    }

    if (s_dpmzm_ctx.pilot_output_enabled) {
        pilot_output_mode = "continuous";
    } else if (temporary_pilot_started) {
        pilot_output_mode = "capture-temp";
    }

    board_delay_ms((uint32_t)settle_ms);
    printf("[dpmzm] capture start: samples=%lu settle=%lu ms source=%s mode=%s\r\n",
           requested_samples,
           settle_ms,
           pilot_source_name(s_dpmzm_ctx.config->pilot_source),
           pilot_output_mode);

    for (i = 0UL; i < requested_samples; i++) {
        ads131m02_sample_t sample;

        if (!wait_and_read_capture_sample(&sample)) {
            printf("[dpmzm] capture failed at sample %lu\r\n", i);
            capture_end_onboard_pilot(temporary_pilot_started);
            return;
        }

        s_capture_samples[i].ch0_code = sample.ch0;
        s_capture_samples[i].ch1_code = sample.ch1;
    }

    capture_end_onboard_pilot(temporary_pilot_started);

    for (i = 0UL; i < requested_samples; i++) {
        print_capture_raw_line((uint32_t)i,
                               bias_i_v,
                               bias_q_v,
                               bias_p_v,
                               pilot_output_mode,
                               &s_capture_samples[i]);
    }

    printf("DPMZMCAPSUM,raw,%lu,%.6f,%.6f,%.6f,%s\r\n",
           requested_samples,
           (double)bias_i_v,
           (double)bias_q_v,
           (double)bias_p_v,
           pilot_source_name(s_dpmzm_ctx.config->pilot_source));
    printf("[dpmzm] capture done: samples=%lu\r\n", requested_samples);
}

void app_dpmzm_init(void)
{
    app_config_dpmzm_defaults();
    dpmzm_auto_init();
    dpmzm_lock_init();
    s_dpmzm_ctx.config = app_config_dpmzm_get();
    s_dpmzm_ctx.bias_i_v = s_dpmzm_ctx.config->bias_i_initial_v;
    s_dpmzm_ctx.bias_q_v = s_dpmzm_ctx.config->bias_q_initial_v;
    s_dpmzm_ctx.bias_p_v = s_dpmzm_ctx.config->bias_p_initial_v;
    s_dpmzm_ctx.pilot_output_enabled = false;
    reset_pilot_phases();
    s_pilot_timer_running = false;
    s_scan_pilot_active = false;
    s_p_bias_dirty = false;
    s_pending_p_bias_v = s_dpmzm_ctx.bias_p_v;
    s_scan_restore_valid = false;
    s_pilot_dma_active = false;
    s_pilot_dma_frame_count = 0U;
    s_pilot_dma_frame_index = 0U;
    s_pilot_dma_contains_p = false;
    s_pilot_dma_pending_p_v = s_dpmzm_ctx.bias_p_v;
    s_pilot_dma_drop_count = 0U;
    s_pilot_dma_error_count = 0U;
    s_lock_last_cycle_ms = 0U;
    s_lock_cycle_busy = false;
    reset_lock_runtime_state();
    s_dpmzm_ctx.initialized = true;
}

void app_dpmzm_run(void)
{
    const dpmzm_lock_context_t *lock_ctx = dpmzm_lock_get_context();
    dpmzm_lock_request_t req;
    uint32_t now_ms;
    dpmzm_lock_axis_t axis;

    if (lock_ctx == NULL || !lock_ctx->enabled || s_lock_cycle_busy) {
        return;
    }

    fill_lock_request(&req);
    now_ms = HAL_GetTick();
    if (s_lock_last_cycle_ms != 0U &&
        (now_ms - s_lock_last_cycle_ms) < req.loop_interval_ms) {
        return;
    }

    s_lock_cycle_busy = true;
    axis = dpmzm_lock_next_axis();
    printf("[dpmzm][lock] cycle axis=%s\r\n", dpmzm_lock_axis_name(axis));
    if (!run_lock_step_once(axis, true, NULL)) {
        printf("[dpmzm][lock] cycle stopped by fault\r\n");
    }
    s_lock_last_cycle_ms = HAL_GetTick();
    s_lock_cycle_busy = false;
}

int app_dpmzm_sync_bias_outputs(void)
{
    if (!s_dpmzm_ctx.initialized) {
        return -1;
    }

    if (timer_owns_dac_outputs()) {
        return apply_current_drive();
    }

    return apply_biases();
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
    } else if (strcmp(cmd, "debug") == 0 || strcmp(cmd, "scan debug") == 0) {
        print_debug();
    } else if (strncmp(cmd, "set bias ", 9) == 0) {
        handle_set_bias(cmd);
    } else if (strncmp(cmd, "set pilot-src ", 14) == 0) {
        handle_set_pilot_src(cmd);
    } else if (strncmp(cmd, "set pilot-open ", 15) == 0) {
        handle_set_pilot_open(cmd);
    } else if (strncmp(cmd, "set pilot ", 10) == 0) {
        handle_set_pilot(cmd);
    } else if (strncmp(cmd, "set dump ", 9) == 0) {
        handle_set_dump(cmd);
    } else if (strncmp(cmd, "capture raw ", 12) == 0) {
        handle_capture_raw(cmd);
    } else if (strcmp(cmd, "auto lock") == 0 ||
               strncmp(cmd, "auto lock ", 10) == 0 ||
               strcmp(cmd, "auto run") == 0 ||
               strncmp(cmd, "auto run ", 9) == 0) {
        handle_auto_lock(cmd);
    } else if (strcmp(cmd, "auto coarse") == 0) {
        handle_auto_coarse();
    } else if (strcmp(cmd, "auto fine") == 0) {
        handle_auto_fine();
    } else if (strcmp(cmd, "auto status") == 0) {
        handle_auto_status();
    } else if (strncmp(cmd, "lock probe ", 11) == 0) {
        handle_lock_probe(cmd);
    } else if (strncmp(cmd, "lock step ", 10) == 0) {
        handle_lock_step(cmd);
    } else if (strcmp(cmd, "lock start") == 0) {
        handle_lock_start();
    } else if (strcmp(cmd, "lock stop") == 0) {
        handle_lock_stop();
    } else if (strcmp(cmd, "lock status") == 0) {
        handle_lock_status();
    } else if (strncmp(cmd, "scan matp ", 10) == 0 ||
               strncmp(cmd, "scan qtp ", 9) == 0 ||
               strncmp(cmd, "scan mitp ", 10) == 0) {
        handle_scan_placeholder(cmd);
    } else {
        printf("[dpmzm] unknown command: %s\r\n", cmd);
    }
}

bool app_dpmzm_pilot_output_active(void)
{
    return pilot_generation_active_internal();
}

void app_dpmzm_drive_next_sample(void)
{
    drive_next_sample_internal();
}

void app_dpmzm_pilot_spi_tx_cplt(void)
{
    bool start_next = false;
    uint8_t next_index = 0U;

    board_dac_cs_high();

    __disable_irq();
    if (!s_pilot_dma_active) {
        __enable_irq();
        return;
    }

    s_pilot_dma_frame_index++;
    if (s_pilot_dma_frame_index < s_pilot_dma_frame_count) {
        next_index = s_pilot_dma_frame_index;
        start_next = true;
    } else {
        s_pilot_dma_active = false;
        s_pilot_dma_frame_count = 0U;
        s_pilot_dma_frame_index = 0U;
        s_pilot_dma_contains_p = false;
    }
    __enable_irq();

    if (start_next) {
        if (!start_spi1_dma_frame(next_index)) {
            app_dpmzm_pilot_spi_error();
        }
    } else {
        board_dac_ldac_pulse();
    }
}

void app_dpmzm_pilot_spi_error(void)
{
    bool restore_p = false;
    float restore_p_v = 0.0f;

    board_dac_cs_high();

    __disable_irq();
    restore_p = s_pilot_dma_contains_p;
    restore_p_v = s_pilot_dma_pending_p_v;
    s_pilot_dma_active = false;
    s_pilot_dma_frame_count = 0U;
    s_pilot_dma_frame_index = 0U;
    s_pilot_dma_contains_p = false;
    s_pilot_dma_error_count++;
    __enable_irq();

    if (restore_p) {
        requeue_pending_p_bias(restore_p_v);
    }
}

bool app_dpmzm_scan_begin(bool use_onboard_pilot)
{
    if (!s_dpmzm_ctx.initialized) {
        app_dpmzm_init();
    }

    capture_scan_restore_biases();
    s_scan_pilot_active = false;
    s_scan_reused_continuous_pilot = false;
    s_scan_started_temporary_pilot = false;

    if (use_onboard_pilot &&
        s_dpmzm_ctx.config->pilot_source == DPMZM_PILOT_SOURCE_ONBOARD) {
        s_scan_pilot_active = true;

        /*
         * If the user already has continuous pilot output enabled, scan should
         * reuse that exact running waveform. Stopping and restarting the pilot
         * here changes the experimental condition compared with fixed-point
         * raw capture and can make Goertzel metrics disagree with the raw FFT.
         */
        if (s_dpmzm_ctx.pilot_output_enabled && s_pilot_timer_running) {
            s_scan_reused_continuous_pilot = true;
            return true;
        }

        stop_pilot_timer();
        reset_pilot_phases();
        (void)apply_current_drive();
        if (!start_pilot_timer()) {
            s_scan_pilot_active = false;
            s_scan_reused_continuous_pilot = false;
            restore_scan_biases_to_context();
            (void)apply_biases();
            return false;
        }
        s_scan_started_temporary_pilot = true;
    } else {
        stop_pilot_timer();
        (void)apply_biases();
    }

    return true;
}

void app_dpmzm_scan_end(void)
{
    bool reused_continuous = s_scan_reused_continuous_pilot;

    if (!reused_continuous || s_scan_started_temporary_pilot) {
        stop_pilot_timer();
    }
    s_scan_pilot_active = false;
    s_scan_reused_continuous_pilot = false;
    s_scan_started_temporary_pilot = false;
    reset_pilot_phases();
    restore_scan_biases_to_context();
    if (reused_continuous && s_dpmzm_ctx.pilot_output_enabled && s_pilot_timer_running) {
        requeue_pending_p_bias(s_dpmzm_ctx.bias_p_v);
    } else {
        (void)apply_biases();
    }
    if (!reused_continuous && s_dpmzm_ctx.pilot_output_enabled) {
        if (!start_pilot_timer()) {
            s_dpmzm_ctx.pilot_output_enabled = false;
            reset_pilot_phases();
            (void)apply_biases();
            printf("[dpmzm] WARN: failed to restore continuous pilot after scan\r\n");
        }
    }
}

int app_dpmzm_scan_apply_bias_triplet(float vi, float vq, float vp)
{
    update_bias_triplet_state(vi, vq, vp);

    if (timer_owns_dac_outputs()) {
        return 0;
    }

    return apply_biases();
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) {
        return;
    }

    if (htim->Instance == TIM6) {
        s_pilot_tim6_irq_count++;
        drive_next_sample_internal();
    }
}
