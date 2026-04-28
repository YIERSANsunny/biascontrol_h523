#ifndef CTRL_AUTO_DPMZM_H
#define CTRL_AUTO_DPMZM_H

#include <stdbool.h>
#include <stdint.h>
#include "ctrl_scan_dpmzm.h"

typedef enum {
    DPMZM_AUTO_IDLE = 0,
    DPMZM_AUTO_SCAN_I_MATP,
    DPMZM_AUTO_SCAN_Q_MATP,
    DPMZM_AUTO_PICK_I_PLATEAU,
    DPMZM_AUTO_PICK_Q_PLATEAU,
    DPMZM_AUTO_SCAN_P_QTP,
    DPMZM_AUTO_SCAN_I_MITP,
    DPMZM_AUTO_SCAN_Q_MITP,
    DPMZM_AUTO_FINE_SCAN_P_WIDE,
    DPMZM_AUTO_FINE_SCAN_P_FINE,
    DPMZM_AUTO_FINE_SCAN_I_WIDE,
    DPMZM_AUTO_FINE_SCAN_I_FINE,
    DPMZM_AUTO_FINE_SCAN_Q_WIDE,
    DPMZM_AUTO_FINE_SCAN_Q_FINE,
    DPMZM_AUTO_DONE,
    DPMZM_AUTO_FAILED
} dpmzm_auto_state_t;

typedef enum {
    DPMZM_AUTO_OK = 0,
    DPMZM_AUTO_ERR_BAD_ARG,
    DPMZM_AUTO_ERR_I_MATP_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_MATP_NOT_FOUND,
    DPMZM_AUTO_ERR_I_PLATEAU_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_PLATEAU_NOT_FOUND,
    DPMZM_AUTO_ERR_P_QTP_INVALID,
    DPMZM_AUTO_ERR_I_MITP_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_MITP_NOT_FOUND,
    DPMZM_AUTO_ERR_NO_COARSE_RESULT,
    DPMZM_AUTO_ERR_NEED_WIDER_WINDOW,
    DPMZM_AUTO_ERR_SCAN_EXECUTION
} dpmzm_auto_error_t;

typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    float symmetry_score;
    uint16_t point_index;
    bool valid;
} dpmzm_matp_candidate_t;

typedef struct {
    float left_matp_v;
    float right_matp_v;
    float plateau_left_v;
    float plateau_right_v;
    float plateau_center_v;
    float plateau_peak_dbm;
    float plateau_width_v;
    bool valid;
} dpmzm_sensitive_region_t;

typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    uint16_t point_index;
    bool selected_as_mitp;
    bool valid;
} dpmzm_mitp_candidate_t;

typedef struct {
    dpmzm_scan_request_t scan_template;
    float sweep_min_v;
    float sweep_max_v;
    float sweep_step_v;
} dpmzm_auto_coarse_request_t;

typedef struct {
    float i_matp_ref_v;
    float q_matp_ref_v;
    float i_pqtp_init_v;
    float q_pqtp_init_v;
    float p_qtp_coarse_v;
    float i_mitp_coarse_v;
    float q_mitp_coarse_v;
    bool p_qtp_valid;
    bool i_mitp_valid;
    bool q_mitp_valid;
    dpmzm_auto_error_t error;
} dpmzm_auto_coarse_result_t;

typedef struct {
    dpmzm_scan_request_t scan_template;
    dpmzm_auto_coarse_result_t coarse_result;
    float sweep_min_v;
    float sweep_max_v;
    float wide_range_v;
    float wide_step_v;
    float p_fine_range_v;
    float iq_fine_range_v;
    float expanded_range_v;
    float fine_step_v;
} dpmzm_auto_fine_request_t;

typedef struct {
    float p_qtp_wide_v;
    float i_mitp_wide_v;
    float q_mitp_wide_v;
    float p_qtp_fine_v;
    float i_mitp_fine_v;
    float q_mitp_fine_v;
    bool p_qtp_valid;
    bool i_mitp_valid;
    bool q_mitp_valid;
    bool p_qtp_expanded;
    bool i_mitp_expanded;
    bool q_mitp_expanded;
    dpmzm_auto_error_t error;
} dpmzm_auto_fine_result_t;

typedef struct {
    dpmzm_auto_state_t state;
    dpmzm_auto_error_t error;
    uint32_t retry_count;
    bool has_result;
    bool has_fine_result;
    dpmzm_auto_coarse_request_t last_request;
    dpmzm_auto_coarse_result_t last_result;
    dpmzm_auto_fine_result_t last_fine_result;
} dpmzm_auto_context_t;

void dpmzm_auto_init(void);
const dpmzm_auto_context_t *dpmzm_auto_get_context(void);
bool dpmzm_auto_run_coarse(const dpmzm_auto_coarse_request_t *req,
                           dpmzm_auto_coarse_result_t *out);
bool dpmzm_auto_run_fine(const dpmzm_auto_fine_request_t *req,
                         dpmzm_auto_fine_result_t *out);
void dpmzm_auto_print_status(void);
void dpmzm_auto_print_result(const dpmzm_auto_coarse_result_t *result);
void dpmzm_auto_print_fine_result(const dpmzm_auto_fine_result_t *result);

#endif /* CTRL_AUTO_DPMZM_H */
