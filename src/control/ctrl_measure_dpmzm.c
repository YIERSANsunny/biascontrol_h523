#include "ctrl_measure_dpmzm.h"
#include <math.h>

static float dpmzm_freq_diff_hz(const dpmzm_measure_ctx_t *ctx)
{
    return fabsf(ctx->pilot_i_freq_hz - ctx->pilot_q_freq_hz);
}

static float dpmzm_freq_sum_hz(const dpmzm_measure_ctx_t *ctx)
{
    return ctx->pilot_i_freq_hz + ctx->pilot_q_freq_hz;
}

static bool dpmzm_measure_enabled(const dpmzm_measure_ctx_t *ctx,
                                  uint32_t flag)
{
    return ctx != NULL && ((ctx->enabled_flags & flag) != 0U);
}

static const float s_measure_search_offsets_hz[DPMZM_MEASURE_SEARCH_TONE_COUNT] = {
    -150.0f, -100.0f, -50.0f, 0.0f, 50.0f, 100.0f, 150.0f
};

static float clamp_search_freq_hz(float freq_hz)
{
    return (freq_hz > 1.0f) ? freq_hz : 1.0f;
}

static void init_goertzel_bank(goertzel_state_t bank[DPMZM_MEASURE_SEARCH_TONE_COUNT],
                               float center_freq_hz,
                               float sample_rate_hz,
                               uint32_t block_size)
{
    uint32_t i;

    for (i = 0U; i < DPMZM_MEASURE_SEARCH_TONE_COUNT; i++) {
        goertzel_init(&bank[i],
                      clamp_search_freq_hz(center_freq_hz + s_measure_search_offsets_hz[i]),
                      sample_rate_hz,
                      block_size);
    }
}

static void reset_goertzel_bank(goertzel_state_t bank[DPMZM_MEASURE_SEARCH_TONE_COUNT])
{
    uint32_t i;

    for (i = 0U; i < DPMZM_MEASURE_SEARCH_TONE_COUNT; i++) {
        goertzel_reset(&bank[i]);
    }
}

static void process_goertzel_bank(goertzel_state_t bank[DPMZM_MEASURE_SEARCH_TONE_COUNT],
                                  float sample)
{
    uint32_t i;

    for (i = 0U; i < DPMZM_MEASURE_SEARCH_TONE_COUNT; i++) {
        goertzel_process_sample(&bank[i], sample);
    }
}

static bool goertzel_bank_ready(const goertzel_state_t bank[DPMZM_MEASURE_SEARCH_TONE_COUNT])
{
    uint32_t i;

    for (i = 0U; i < DPMZM_MEASURE_SEARCH_TONE_COUNT; i++) {
        if (!goertzel_block_ready(&bank[i])) {
            return false;
        }
    }
    return true;
}

static float goertzel_bank_max_magnitude(goertzel_state_t bank[DPMZM_MEASURE_SEARCH_TONE_COUNT])
{
    float best_mag = 0.0f;
    float phase = 0.0f;
    uint32_t i;

    for (i = 0U; i < DPMZM_MEASURE_SEARCH_TONE_COUNT; i++) {
        float mag = 0.0f;

        goertzel_get_result(&bank[i], &mag, &phase);
        if (mag > best_mag) {
            best_mag = mag;
        }
    }

    return best_mag;
}

void dpmzm_measure_init(dpmzm_measure_ctx_t *ctx,
                        float pilot_i_freq_hz,
                        float pilot_q_freq_hz,
                        float sample_rate_hz,
                        uint32_t block_size)
{
    dpmzm_measure_init_select(ctx,
                              pilot_i_freq_hz,
                              pilot_q_freq_hz,
                              sample_rate_hz,
                              block_size,
                              DPMZM_MEASURE_ALL);
}

void dpmzm_measure_init_select(dpmzm_measure_ctx_t *ctx,
                               float pilot_i_freq_hz,
                               float pilot_q_freq_hz,
                               float sample_rate_hz,
                               uint32_t block_size,
                               uint32_t enabled_flags)
{
    if (ctx == NULL) {
        return;
    }

    ctx->pilot_i_freq_hz = pilot_i_freq_hz;
    ctx->pilot_q_freq_hz = pilot_q_freq_hz;
    ctx->sample_rate_hz = sample_rate_hz;
    ctx->block_size = block_size;
    ctx->enabled_flags = enabled_flags;

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        init_goertzel_bank(ctx->g_fi, pilot_i_freq_hz, sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        init_goertzel_bank(ctx->g_fq, pilot_q_freq_hz, sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        init_goertzel_bank(ctx->g_fdiff, dpmzm_freq_diff_hz(ctx), sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        init_goertzel_bank(ctx->g_fsum, dpmzm_freq_sum_hz(ctx), sample_rate_hz, block_size);
    }
    dc_accum_init(&ctx->dc_acc, block_size);
}

void dpmzm_measure_reset(dpmzm_measure_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        reset_goertzel_bank(ctx->g_fi);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        reset_goertzel_bank(ctx->g_fq);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        reset_goertzel_bank(ctx->g_fdiff);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        reset_goertzel_bank(ctx->g_fsum);
    }
    dc_accum_reset(&ctx->dc_acc);
}

void dpmzm_measure_process_sample(dpmzm_measure_ctx_t *ctx,
                                  float sample_ac,
                                  float sample_dc)
{
    if (ctx == NULL) {
        return;
    }

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        process_goertzel_bank(ctx->g_fi, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        process_goertzel_bank(ctx->g_fq, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        process_goertzel_bank(ctx->g_fdiff, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        process_goertzel_bank(ctx->g_fsum, sample_ac);
    }
    dc_accum_process(&ctx->dc_acc, sample_dc);
}

bool dpmzm_measure_block_ready(const dpmzm_measure_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI) &&
        !goertzel_bank_ready(ctx->g_fi)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ) &&
        !goertzel_bank_ready(ctx->g_fq)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF) &&
        !goertzel_bank_ready(ctx->g_fdiff)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM) &&
        !goertzel_bank_ready(ctx->g_fsum)) {
        return false;
    }

    return dc_accum_ready(&ctx->dc_acc);
}

bool dpmzm_measure_finalize(dpmzm_measure_ctx_t *ctx,
                            dpmzm_measurement_t *out)
{
    if (ctx == NULL || out == NULL || !dpmzm_measure_block_ready(ctx)) {
        return false;
    }

    out->mag_fi = 0.0f;
    out->mag_fq = 0.0f;
    out->mag_fdiff = 0.0f;
    out->mag_fsum = 0.0f;

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        out->mag_fi = goertzel_bank_max_magnitude(ctx->g_fi);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        out->mag_fq = goertzel_bank_max_magnitude(ctx->g_fq);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        out->mag_fdiff = goertzel_bank_max_magnitude(ctx->g_fdiff);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        out->mag_fsum = goertzel_bank_max_magnitude(ctx->g_fsum);
    }
    out->dc_mean = dc_accum_get_mean(&ctx->dc_acc);
    out->sample_count = ctx->block_size;

    return true;
}

bool dpmzm_measure_block(dpmzm_measure_ctx_t *ctx,
                         const float *samples_ac,
                         const float *samples_dc,
                         uint32_t sample_count,
                         dpmzm_measurement_t *out)
{
    uint32_t i;

    if (ctx == NULL || samples_ac == NULL || samples_dc == NULL || out == NULL) {
        return false;
    }

    if (sample_count != ctx->block_size) {
        return false;
    }

    dpmzm_measure_reset(ctx);
    for (i = 0; i < sample_count; i++) {
        dpmzm_measure_process_sample(ctx, samples_ac[i], samples_dc[i]);
    }

    return dpmzm_measure_finalize(ctx, out);
}
