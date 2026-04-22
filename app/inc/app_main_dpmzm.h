#ifndef APP_MAIN_DPMZM_H
#define APP_MAIN_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "app_config_dpmzm.h"

/**
 * DPMZM open-loop application context.
 *
 * This context is intentionally independent from the legacy app_context_t so
 * the DPMZM experiment path can evolve without changing the validated MZM
 * control path.
 */
typedef struct {
    app_config_dpmzm_t *config;
    float bias_i_v;
    float bias_q_v;
    float bias_p_v;
    bool pilot_output_enabled;
    bool initialized;
} app_dpmzm_context_t;

/**
 * Initialize the DPMZM open-loop application context.
 */
void app_dpmzm_init(void);

/**
 * Run one non-blocking DPMZM application iteration.
 *
 * The first open-loop version does not have a background state machine.
 * Continuous onboard pilot generation is driven by TIM6 interrupt, so this
 * function currently remains a placeholder for future non-ISR tasks.
 */
void app_dpmzm_run(void);

/**
 * Handle a DPMZM-specific UART command.
 */
void app_dpmzm_handle_command(const char *cmd);

/**
 * Get the DPMZM application context for monitoring/debug.
 */
const app_dpmzm_context_t *app_dpmzm_get_context(void);

/**
 * Return true when continuous onboard DPMZM pilot output is enabled.
 */
bool app_dpmzm_pilot_output_active(void);

/**
 * Advance one pilot sample and write the current I/Q outputs to DAC.
 *
 * This is primarily intended for the TIM6 interrupt path. It remains exposed
 * so bench tools can force a single update when needed.
 */
void app_dpmzm_drive_next_sample(void);

#endif /* APP_MAIN_DPMZM_H */
