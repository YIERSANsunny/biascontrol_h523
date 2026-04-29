#ifndef DSP_TYPES_H
#define DSP_TYPES_H

#include <stdint.h>

/* ========================================================================= */
/*  DSP system parameters                                                    */
/* ========================================================================= */

/** ADC sample rate in Hz (ADS131M02, OSR=128, HR mode, measured scan path). */
#define DSP_SAMPLE_RATE_HZ      32000

/** Pilot tone frequency in Hz */
#define DSP_PILOT_FREQ_HZ            1000

/** Samples per 1 kHz pilot cycle at 32 kSPS. */
#define DSP_PILOT_PERIOD_SAMPLES     32

/** Number of full pilot cycles integrated by each Goertzel block.
 *  Using multiple cycles improves SNR for weak harmonic extraction while
 *  preserving integer-cycle coherence (no spectral leakage). */
#define DSP_GOERTZEL_BLOCK_CYCLES    20

/** Goertzel block size in samples.
 *  N = 32 samples/cycle * 20 cycles = 640 samples = 20 ms @ 32 kSPS. */
#define DSP_GOERTZEL_BLOCK_SIZE      (DSP_PILOT_PERIOD_SAMPLES * DSP_GOERTZEL_BLOCK_CYCLES)

/** Number of Goertzel blocks per control update.
 *  With N=640 and decimation=10, control rate is 32k / (640 * 10) = 5 Hz. */
#define DSP_CONTROL_DECIMATION       10

/* ========================================================================= */
/*  Harmonic analysis result — shared between DSP and Control layers         */
/* ========================================================================= */

typedef struct {
    float h1_magnitude;  /**< 1st harmonic magnitude (at pilot freq f0) */
    float h2_magnitude;  /**< 2nd harmonic magnitude (at 2*f0) */
    float h1_phase;      /**< 1st harmonic phase relative to pilot (radians) */
    float h2_phase;      /**< 2nd harmonic phase (radians) */
    float dc_power;      /**< DC component from CH1 (mean of block samples) */
} harmonic_data_t;

#endif /* DSP_TYPES_H */
