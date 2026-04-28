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
        goertzel_init(&ctx->g_fi, pilot_i_freq_hz, sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        goertzel_init(&ctx->g_fq, pilot_q_freq_hz, sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        goertzel_init(&ctx->g_fdiff, dpmzm_freq_diff_hz(ctx), sample_rate_hz, block_size);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        goertzel_init(&ctx->g_fsum, dpmzm_freq_sum_hz(ctx), sample_rate_hz, block_size);
    }
    dc_accum_init(&ctx->dc_acc, block_size);
}

void dpmzm_measure_reset(dpmzm_measure_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        goertzel_reset(&ctx->g_fi);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        goertzel_reset(&ctx->g_fq);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        goertzel_reset(&ctx->g_fdiff);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        goertzel_reset(&ctx->g_fsum);
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
        goertzel_process_sample(&ctx->g_fi, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        goertzel_process_sample(&ctx->g_fq, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        goertzel_process_sample(&ctx->g_fdiff, sample_ac);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        goertzel_process_sample(&ctx->g_fsum, sample_ac);
    }
    dc_accum_process(&ctx->dc_acc, sample_dc);
}

bool dpmzm_measure_block_ready(const dpmzm_measure_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI) &&
        !goertzel_block_ready(&ctx->g_fi)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ) &&
        !goertzel_block_ready(&ctx->g_fq)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF) &&
        !goertzel_block_ready(&ctx->g_fdiff)) {
        return false;
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM) &&
        !goertzel_block_ready(&ctx->g_fsum)) {
        return false;
    }

    return dc_accum_ready(&ctx->dc_acc);
}

bool dpmzm_measure_finalize(dpmzm_measure_ctx_t *ctx,
                            dpmzm_measurement_t *out)
{
    float phase = 0.0f;

    if (ctx == NULL || out == NULL || !dpmzm_measure_block_ready(ctx)) {
        return false;
    }

    out->mag_fi = 0.0f;
    out->mag_fq = 0.0f;
    out->mag_fdiff = 0.0f;
    out->mag_fsum = 0.0f;

    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FI)) {
        goertzel_get_result(&ctx->g_fi, &out->mag_fi, &phase);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FQ)) {
        goertzel_get_result(&ctx->g_fq, &out->mag_fq, &phase);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FDIFF)) {
        goertzel_get_result(&ctx->g_fdiff, &out->mag_fdiff, &phase);
    }
    if (dpmzm_measure_enabled(ctx, DPMZM_MEASURE_FSUM)) {
        goertzel_get_result(&ctx->g_fsum, &out->mag_fsum, &phase);
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
