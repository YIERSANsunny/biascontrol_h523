#ifndef CTRL_MEASURE_DPMZM_H
#define CTRL_MEASURE_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "dsp_goertzel.h"

/**
 * DPMZM multi-frequency measurement result for one coherent block.
 *
 * The first open-loop version focuses on amplitudes plus a DC mean because
 * they are the minimum set required to reproduce MATP/QTP/MITP scan curves.
 */
typedef struct {
    float mag_fi;
    float mag_fq;
    float mag_fdiff;
    float mag_fsum;
    float dc_mean;
    uint32_t sample_count;
} dpmzm_measurement_t;

/**
 * DPMZM multi-frequency Goertzel measurement context.
 */
typedef struct {
    goertzel_state_t g_fi;
    goertzel_state_t g_fq;
    goertzel_state_t g_fdiff;
    goertzel_state_t g_fsum;
    dc_accum_t dc_acc;
    float pilot_i_freq_hz;
    float pilot_q_freq_hz;
    float sample_rate_hz;
    uint32_t block_size;
} dpmzm_measure_ctx_t;

/**
 * Initialize a DPMZM measurement context.
 */
void dpmzm_measure_init(dpmzm_measure_ctx_t *ctx,
                        float pilot_i_freq_hz,
                        float pilot_q_freq_hz,
                        float sample_rate_hz,
                        uint32_t block_size);

/**
 * Reset the internal accumulators for a new coherent block.
 */
void dpmzm_measure_reset(dpmzm_measure_ctx_t *ctx);

/**
 * Feed one AC/DC sample pair into the DPMZM measurement context.
 */
void dpmzm_measure_process_sample(dpmzm_measure_ctx_t *ctx,
                                  float sample_ac,
                                  float sample_dc);

/**
 * Check whether the coherent block is complete.
 */
bool dpmzm_measure_block_ready(const dpmzm_measure_ctx_t *ctx);

/**
 * Finalize the current coherent block into a measurement result.
 *
 * Returns false if the block is incomplete or the arguments are invalid.
 */
bool dpmzm_measure_finalize(dpmzm_measure_ctx_t *ctx,
                            dpmzm_measurement_t *out);

/**
 * Convenience helper: process a full AC/DC block in one call.
 *
 * sample_count must equal the configured block size.
 */
bool dpmzm_measure_block(dpmzm_measure_ctx_t *ctx,
                         const float *samples_ac,
                         const float *samples_dc,
                         uint32_t sample_count,
                         dpmzm_measurement_t *out);

#endif /* CTRL_MEASURE_DPMZM_H */
