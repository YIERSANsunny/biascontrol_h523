#include "app_config_dpmzm.h"

static app_config_dpmzm_t s_dpmzm_config;

void app_config_dpmzm_defaults(void)
{
    /*
     * Default to the first three DAC outputs.
     *
     * IMPORTANT:
     * These are only provisional channel defaults for the first DPMZM bring-up
     * pass. The earlier planning documents did not freeze the exact physical
     * mapping of I/Q/P onto VA/VB/VC/..., so this 0/1/2 assignment must be
     * treated as a placeholder until the hardware hookup is confirmed.
     */
    s_dpmzm_config.bias_i_dac_channel = 0U;
    s_dpmzm_config.bias_q_dac_channel = 1U;
    s_dpmzm_config.bias_p_dac_channel = 2U;

    s_dpmzm_config.bias_i_initial_v = 0.0f;
    s_dpmzm_config.bias_q_initial_v = 0.0f;
    s_dpmzm_config.bias_p_initial_v = 0.0f;

    /*
     * Match the currently validated DPMZM experiment plan:
     *   fI = 1000 Hz
     *   fQ = 1200 Hz
     * with default pilot amplitudes raised to 200 mVpp
     * (stored here as 100 mV peak values).
     *
     * The pilot source defaults to ONBOARD because the current requirement is
     * to generate both pilots with the DAC path rather than relying on
     * external signal sources.
     */
    s_dpmzm_config.pilot_i_freq_hz = 1000.0f;
    s_dpmzm_config.pilot_q_freq_hz = 1200.0f;
    s_dpmzm_config.pilot_i_amp_v = 0.10f;
    s_dpmzm_config.pilot_q_amp_v = 0.10f;
    s_dpmzm_config.pilot_source = DPMZM_PILOT_SOURCE_ONBOARD;

    /*
     * Reuse the current 5 Hz measurement cadence as the starting point for
     * open-loop scans.
     */
    s_dpmzm_config.scan_default_blocks = 10U;
    s_dpmzm_config.dump_mode = DPMZM_DUMP_METRICS;
}

app_config_dpmzm_t *app_config_dpmzm_get(void)
{
    return &s_dpmzm_config;
}
