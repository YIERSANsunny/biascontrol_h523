#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "app_state.h"
#include "app_config.h"
#include "ctrl_bias.h"

/**
 * Application context — top-level structure holding all runtime state.
 */
typedef struct {
    app_state_t state;
    bias_ctrl_t bias_ctrl;
    app_config_t *config;
    uint32_t tick_ms;           /**< System tick counter */
    uint32_t state_enter_ms;    /**< Tick when current state was entered */
} app_context_t;

/**
 * Initialize the application. Called once from main().
 * Sets up all hardware, drivers, and enters INIT state.
 */
void app_init(void);

/**
 * Run one iteration of the application state machine.
 * Called from the main loop (non-blocking).
 */
void app_run(void);

/**
 * Get the application context (for debug/monitoring).
 */
const app_context_t *app_get_context(void);

/**
 * Handle a command from the UART debug interface.
 * Called when a complete command line is received.
 *
 * Supported commands:
 *   "mode status"        — print active runtime namespace (mzm/dpmzm/idle)
 *   "mode mzm"           — route bare commands to the legacy MZM app
 *   "mode dpmzm"         — route bare commands to the DPMZM app
 *   "mode idle"          — stop active control paths and keep shell alive
 *   "mzm <cmd>"          — explicitly run a legacy MZM command
 *   "dpmzm <cmd>"        — explicitly run a DPMZM command
 *   "start"              — begin closed-loop bias control
 *   "stop"               — stop bias control
 *   "status"             — print state, bias voltage, lock status
 *   "set mod <type>"     — change modulator type (mzm, ...)
 *   "set bp <point>"     — change target bias point (quad, max, min)
 *   "set pilot <mVpp>"   — set pilot amplitude in mV peak-to-peak (e.g. 100)
 *   "dac <V>"            — set DAC channel A to voltage (static, no pilot)
 *   "dac mid"            — set all DAC channels to 0 V
 *   "adc [N]"            — read N ADC samples (default 64), print raw values
 *   "goertzel [N]"       — run N Goertzel blocks, print H1/H2/DC statistics
 *   "cal bias"           — force a fresh bias calibration scan and update anchors
 *   "scan vpi [fast]"    — V_pi characterization sweep; "fast" = single-sided
 *   "scan harmonics [fast] [blocks]" — open-loop H1/H2/DC sweep with signed harmonics
 *
 * @param cmd  Null-terminated command string
 */
void app_handle_command(const char *cmd);

#endif /* APP_MAIN_H */
