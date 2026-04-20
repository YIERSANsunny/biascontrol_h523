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
    bool initialized;
} app_dpmzm_context_t;

/**
 * Initialize the DPMZM open-loop application context.
 */
void app_dpmzm_init(void);

/**
 * Run one non-blocking DPMZM application iteration.
 *
 * The first open-loop version does not have an internal state machine yet, so
 * this function is currently a placeholder for future background tasks.
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

#endif /* APP_MAIN_DPMZM_H */
