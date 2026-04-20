#include <math.h>
#include <stdio.h>
#include "ctrl_measure_dpmzm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 64000.0f
#define BLOCK_SIZE  1280u
#define FREQ_I      1000.0f
#define FREQ_Q      1200.0f
#define AMP_I       0.80f
#define AMP_Q       0.35f
#define AMP_DIFF    0.22f
#define AMP_SUM     0.15f
#define DC_LEVEL    1.70f
#define TOLERANCE   0.02f

static int tests_passed = 0;
static int tests_failed = 0;

static void check_close(const char *name, float actual, float expected, float tol)
{
    float diff = fabsf(actual - expected);
    if (diff <= tol) {
        tests_passed++;
        printf("  PASS: %s = %.4f (expected %.4f)\n", name, actual, expected);
    } else {
        tests_failed++;
        printf("  FAIL: %s = %.4f (expected %.4f, diff %.4f)\n",
               name, actual, expected, diff);
    }
}

int main(void)
{
    float samples_ac[BLOCK_SIZE];
    float samples_dc[BLOCK_SIZE];
    dpmzm_measure_ctx_t ctx;
    dpmzm_measurement_t out;
    uint32_t i;

    printf("=== DPMZM Multi-Frequency Measurement Test ===\n");

    for (i = 0; i < BLOCK_SIZE; i++) {
        float t = (float)i / SAMPLE_RATE;
        samples_ac[i] =
            AMP_I    * sinf(2.0f * (float)M_PI * FREQ_I * t) +
            AMP_Q    * sinf(2.0f * (float)M_PI * FREQ_Q * t) +
            AMP_DIFF * sinf(2.0f * (float)M_PI * fabsf(FREQ_I - FREQ_Q) * t) +
            AMP_SUM  * sinf(2.0f * (float)M_PI * (FREQ_I + FREQ_Q) * t);

        /*
         * Add a coherent AC term on the DC channel. Over the 1280-sample window
         * it averages to zero, so the mean should still equal DC_LEVEL.
         */
        samples_dc[i] =
            DC_LEVEL +
            0.25f * sinf(2.0f * (float)M_PI * FREQ_I * t);
    }

    dpmzm_measure_init(&ctx, FREQ_I, FREQ_Q, SAMPLE_RATE, BLOCK_SIZE);

    if (!dpmzm_measure_block(&ctx, samples_ac, samples_dc, BLOCK_SIZE, &out)) {
        printf("  FAIL: dpmzm_measure_block() returned false\n");
        return 1;
    }

    check_close("mag_fi", out.mag_fi, AMP_I, TOLERANCE);
    check_close("mag_fq", out.mag_fq, AMP_Q, TOLERANCE);
    check_close("mag_fdiff", out.mag_fdiff, AMP_DIFF, TOLERANCE);
    check_close("mag_fsum", out.mag_fsum, AMP_SUM, TOLERANCE);
    check_close("dc_mean", out.dc_mean, DC_LEVEL, TOLERANCE);

    if (out.sample_count == BLOCK_SIZE) {
        tests_passed++;
        printf("  PASS: sample_count = %lu\n", (unsigned long)out.sample_count);
    } else {
        tests_failed++;
        printf("  FAIL: sample_count = %lu (expected %u)\n",
               (unsigned long)out.sample_count, (unsigned)BLOCK_SIZE);
    }

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
