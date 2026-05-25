#ifndef CTRL_DPMZM_DSP_H
#define CTRL_DPMZM_DSP_H

/*
 * DPMZM measurement constants intentionally stay on the validated 32 kSPS
 * path from dpmzm-open-loop.  The integration branch may use a different
 * global DSP rate for MZM, but DPMZM Goertzel bins and scan blocks must remain
 * coherent with the ADC data path that was experimentally verified.
 */
#define DPMZM_DSP_SAMPLE_RATE_HZ            32000U
#define DPMZM_DSP_PILOT_PERIOD_SAMPLES      32U
#define DPMZM_DSP_GOERTZEL_BLOCK_CYCLES     20U
#define DPMZM_DSP_GOERTZEL_BLOCK_SIZE       (DPMZM_DSP_PILOT_PERIOD_SAMPLES * \
                                             DPMZM_DSP_GOERTZEL_BLOCK_CYCLES)
#define DPMZM_DSP_CONTROL_DECIMATION        10U

#endif /* CTRL_DPMZM_DSP_H */
