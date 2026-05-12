#include "ctrl_auto_dpmzm.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DPMZM_AUTO_SCAN_POINTS_MAX            256U
#define DPMZM_AUTO_MATP_CANDIDATES_MAX        4U
#define DPMZM_AUTO_MITP_CANDIDATES_MAX        4U
#define DPMZM_AUTO_MIN_PROMINENCE_DB          2.0f
#define DPMZM_AUTO_MIN_QTP_RANGE_DB           3.0f
#define DPMZM_AUTO_PLATEAU_DROP_DB            1.0f
#define DPMZM_AUTO_MIN_SEPARATION_MULTIPLIER  6.0f
#define DPMZM_AUTO_MIN_SEPARATION_ABS_V       0.8f
#define DPMZM_AUTO_EDGE_MARGIN_POINTS         5U
#define DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS    2U
#define DPMZM_AUTO_EDGE_VALLEY_RISE_DB        4.0f
#define DPMZM_AUTO_QTP_SPIKE_REJECT_DB        18.0f
#define DPMZM_AUTO_QTP_POSITIVE_MARGIN_DB     3.0f
#define DPMZM_AUTO_MITP_DC_SELECT_WINDOW_DB   3.0f
#define DPMZM_AUTO_DBM_FLOOR_MW               1e-15f
#define DPMZM_AUTO_MATP_PLATFORM_THRESHOLD    0.65f
#define DPMZM_AUTO_MATP_PLATFORM_MIN_POINTS   3U
#define DPMZM_AUTO_MATP_PLATFORM_MIN_WIDTH_V  1.0f
#define DPMZM_AUTO_MATP_PLATFORM_WIDTH_REF_V  2.0f
#define DPMZM_AUTO_MATP_PLATFORM_WIDTH_WEIGHT 0.2f
#define DPMZM_AUTO_TURN_STEP_V                0.10f
#define DPMZM_AUTO_TURN_FINAL_WINDOW_V        0.10f
#define DPMZM_AUTO_PRELOCK_IQ_WINDOW_V        0.08f
#define DPMZM_AUTO_P_TURN_MAX_SHIFTS          8
#define DPMZM_AUTO_IQ_TURN_MAX_SHIFTS         (-1)

static dpmzm_auto_context_t s_auto_ctx;
static dpmzm_scan_point_t s_scan_points[DPMZM_AUTO_SCAN_POINTS_MAX];

typedef struct {
    uint32_t point_index;
    float bias_v;
    bool valid;
    bool virtual_edge;
} dpmzm_matp_anchor_t;

static const char *auto_state_name(dpmzm_auto_state_t state)
{
    switch (state) {
    case DPMZM_AUTO_IDLE:
        return "IDLE";
    case DPMZM_AUTO_SCAN_I_MATP:
        return "SCAN_I_MATP";
    case DPMZM_AUTO_SCAN_Q_MATP:
        return "SCAN_Q_MATP";
    case DPMZM_AUTO_PICK_I_PLATEAU:
        return "PICK_I_PLATEAU";
    case DPMZM_AUTO_PICK_Q_PLATEAU:
        return "PICK_Q_PLATEAU";
    case DPMZM_AUTO_SCAN_P_QTP:
        return "SCAN_P_QTP";
    case DPMZM_AUTO_SCAN_I_MITP:
        return "SCAN_I_MITP";
    case DPMZM_AUTO_SCAN_Q_MITP:
        return "SCAN_Q_MITP";
    case DPMZM_AUTO_FINE_SCAN_P_WIDE:
        return "FINE_SCAN_P_WIDE";
    case DPMZM_AUTO_FINE_SCAN_P_FINE:
        return "FINE_SCAN_P_FINE";
    case DPMZM_AUTO_FINE_SCAN_I_WIDE:
        return "FINE_SCAN_I_WIDE";
    case DPMZM_AUTO_FINE_SCAN_I_FINE:
        return "FINE_SCAN_I_FINE";
    case DPMZM_AUTO_FINE_SCAN_Q_WIDE:
        return "FINE_SCAN_Q_WIDE";
    case DPMZM_AUTO_FINE_SCAN_Q_FINE:
        return "FINE_SCAN_Q_FINE";
    case DPMZM_AUTO_DONE:
        return "DONE";
    case DPMZM_AUTO_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

static const char *auto_error_name(dpmzm_auto_error_t error)
{
    switch (error) {
    case DPMZM_AUTO_OK:
        return "OK";
    case DPMZM_AUTO_ERR_BAD_ARG:
        return "BAD_ARG";
    case DPMZM_AUTO_ERR_I_MATP_NOT_FOUND:
        return "I_MATP_NOT_FOUND";
    case DPMZM_AUTO_ERR_Q_MATP_NOT_FOUND:
        return "Q_MATP_NOT_FOUND";
    case DPMZM_AUTO_ERR_I_PLATEAU_NOT_FOUND:
        return "I_PLATEAU_NOT_FOUND";
    case DPMZM_AUTO_ERR_Q_PLATEAU_NOT_FOUND:
        return "Q_PLATEAU_NOT_FOUND";
    case DPMZM_AUTO_ERR_P_QTP_INVALID:
        return "P_QTP_INVALID";
    case DPMZM_AUTO_ERR_I_MITP_NOT_FOUND:
        return "I_MITP_NOT_FOUND";
    case DPMZM_AUTO_ERR_Q_MITP_NOT_FOUND:
        return "Q_MITP_NOT_FOUND";
    case DPMZM_AUTO_ERR_NO_COARSE_RESULT:
        return "NO_COARSE_RESULT";
    case DPMZM_AUTO_ERR_NEED_WIDER_WINDOW:
        return "NEED_WIDER_WINDOW";
    case DPMZM_AUTO_ERR_SCAN_EXECUTION:
        return "SCAN_EXECUTION";
    default:
        return "UNKNOWN";
    }
}

static void set_state(dpmzm_auto_state_t state)
{
    s_auto_ctx.state = state;
    printf("[dpmzm][auto] state=%s\r\n", auto_state_name(state));
}

static float metric_from_point(dpmzm_scan_stage_t stage,
                               dpmzm_scan_target_t target,
                               const dpmzm_scan_point_t *point)
{
    if (point == NULL) {
        return FLT_MAX;
    }
    if (stage == DPMZM_SCAN_STAGE_QTP) {
        return point->mag_fsum;
    }
    return (target == DPMZM_SCAN_TARGET_Q) ? point->mag_fq : point->mag_fi;
}

static float peak_metric_to_dbm(float vpeak)
{
    const float vrms = vpeak * 0.70710678f;
    float power_mw = ((vrms * vrms) / 50.0f) * 1000.0f;

    if (power_mw < DPMZM_AUTO_DBM_FLOOR_MW) {
        power_mw = DPMZM_AUTO_DBM_FLOOR_MW;
    }
    return 10.0f * log10f(power_mw);
}

static float metric_dbm_from_point(dpmzm_scan_stage_t stage,
                                   dpmzm_scan_target_t target,
                                   const dpmzm_scan_point_t *point)
{
    return peak_metric_to_dbm(metric_from_point(stage, target, point));
}

static float smoothed_metric_dbm(dpmzm_scan_stage_t stage,
                                 dpmzm_scan_target_t target,
                                 const dpmzm_scan_point_t *points,
                                 uint32_t point_count,
                                 uint32_t index)
{
    uint32_t start = (index > 0U) ? (index - 1U) : 0U;
    uint32_t end = ((index + 1U) < point_count) ? (index + 1U) : (point_count - 1U);
    float sum_dbm = 0.0f;
    uint32_t i;
    uint32_t n = 0U;

    for (i = start; i <= end; i++) {
        sum_dbm += metric_dbm_from_point(stage, target, &points[i]);
        n++;
    }

    return (n > 0U) ? (sum_dbm / (float)n) : -120.0f;
}

static bool scan_candidate_index_allowed(uint32_t point_count, uint32_t index)
{
    return point_count > (2U * DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS) &&
           index >= DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS &&
           (index + DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS) < point_count;
}

static uint32_t expected_point_count(float start_v, float stop_v, float step_v)
{
    float step_sign = (stop_v >= start_v) ? fabsf(step_v) : -fabsf(step_v);
    float sweep = 0.0f;
    uint32_t count = 0U;

    if (step_v == 0.0f) {
        return 0U;
    }

    for (sweep = start_v;
         (step_sign > 0.0f) ? (sweep <= stop_v + 1e-6f) : (sweep >= stop_v - 1e-6f);
         sweep += step_sign) {
        count++;
    }

    return count;
}

static void reset_result(dpmzm_auto_coarse_result_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->error = DPMZM_AUTO_OK;
}

static void reset_fine_result(dpmzm_auto_fine_result_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->error = DPMZM_AUTO_OK;
}

static void clear_context(void)
{
    memset(&s_auto_ctx, 0, sizeof(s_auto_ctx));
    s_auto_ctx.state = DPMZM_AUTO_IDLE;
    s_auto_ctx.error = DPMZM_AUTO_OK;
}

static bool request_valid(const dpmzm_auto_coarse_request_t *req)
{
    if (req == NULL) {
        return false;
    }
    if (req->sweep_step_v == 0.0f) {
        return false;
    }
    if (req->scan_template.blocks == 0U) {
        return false;
    }
    if (expected_point_count(req->sweep_min_v,
                             req->sweep_max_v,
                             req->sweep_step_v) > DPMZM_AUTO_SCAN_POINTS_MAX) {
        return false;
    }
    return true;
}

static bool fine_request_valid(const dpmzm_auto_fine_request_t *req)
{
    if (req == NULL) {
        return false;
    }
    if (req->wide_step_v == 0.0f || req->fine_step_v == 0.0f) {
        return false;
    }
    if (req->wide_range_v <= 0.0f ||
        req->p_fine_range_v <= 0.0f ||
        req->iq_fine_range_v <= 0.0f) {
        return false;
    }
    if (req->scan_template.blocks == 0U) {
        return false;
    }
    if (!req->coarse_result.p_qtp_valid ||
        !req->coarse_result.i_mitp_valid ||
        !req->coarse_result.q_mitp_valid ||
        req->coarse_result.error != DPMZM_AUTO_OK) {
        return false;
    }
    if (expected_point_count(-req->wide_range_v,
                             req->wide_range_v,
                             req->wide_step_v) > DPMZM_AUTO_SCAN_POINTS_MAX) {
        return false;
    }
    if (expected_point_count(-req->p_fine_range_v,
                             req->p_fine_range_v,
                             req->fine_step_v) > DPMZM_AUTO_SCAN_POINTS_MAX) {
        return false;
    }
    if (expected_point_count(-req->iq_fine_range_v,
                             req->iq_fine_range_v,
                             req->fine_step_v) > DPMZM_AUTO_SCAN_POINTS_MAX) {
        return false;
    }
    return true;
}

static uint32_t auto_blocks_for_stage(uint32_t fallback_blocks,
                                      uint32_t iq_blocks,
                                      uint32_t p_blocks,
                                      dpmzm_scan_stage_t stage)
{
    if (stage == DPMZM_SCAN_STAGE_QTP) {
        return (p_blocks > 0U) ? p_blocks : fallback_blocks;
    }
    return (iq_blocks > 0U) ? iq_blocks : fallback_blocks;
}

static void set_auto_scan_blocks(dpmzm_scan_request_t *scan_req,
                                 dpmzm_scan_stage_t stage,
                                 uint32_t iq_blocks,
                                 uint32_t p_blocks)
{
    if (scan_req == NULL) {
        return;
    }

    scan_req->blocks = auto_blocks_for_stage(scan_req->blocks,
                                             iq_blocks,
                                             p_blocks,
                                             stage);
}

static void sort_matp_by_metric(dpmzm_matp_candidate_t *candidates, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < count; i++) {
        for (j = i + 1U; j < count; j++) {
            if (candidates[j].metric_f1_dbm < candidates[i].metric_f1_dbm) {
                dpmzm_matp_candidate_t tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }
}

static void sort_mitp_by_metric(dpmzm_mitp_candidate_t *candidates, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < count; i++) {
        for (j = i + 1U; j < count; j++) {
            if (candidates[j].metric_f1_dbm < candidates[i].metric_f1_dbm) {
                dpmzm_mitp_candidate_t tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }
}

static bool candidate_far_enough(float bias_v,
                                 const dpmzm_matp_candidate_t *candidates,
                                 uint32_t count,
                                 float min_sep_v)
{
    uint32_t i;

    for (i = 0U; i < count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        if (fabsf(candidates[i].bias_v - bias_v) < min_sep_v) {
            return false;
        }
    }

    return true;
}

static float matp_min_separation_v(float step_v)
{
    return fmaxf(DPMZM_AUTO_MIN_SEPARATION_ABS_V,
                 fabsf(step_v) * DPMZM_AUTO_MIN_SEPARATION_MULTIPLIER);
}

static bool bias_near_any_matp(float bias_v,
                               const dpmzm_matp_candidate_t *matp_candidates,
                               uint32_t matp_count,
                               float guard_v)
{
    uint32_t i;

    if (matp_candidates == NULL) {
        return false;
    }

    for (i = 0U; i < matp_count; i++) {
        if (!matp_candidates[i].valid) {
            continue;
        }
        if (fabsf(matp_candidates[i].bias_v - bias_v) < guard_v) {
            return true;
        }
    }

    return false;
}

static uint32_t extract_matp_candidates(const dpmzm_scan_point_t *points,
                                        uint32_t point_count,
                                        dpmzm_scan_target_t target,
                                        float step_v,
                                        dpmzm_matp_candidate_t *out,
                                        uint32_t out_capacity)
{
    dpmzm_matp_candidate_t raw[DPMZM_AUTO_MATP_CANDIDATES_MAX * 4U];
    uint32_t raw_count = 0U;
    uint32_t out_count = 0U;
    uint32_t i;
    float min_sep_v = matp_min_separation_v(step_v);

    memset(raw, 0, sizeof(raw));
    memset(out, 0, sizeof(*out) * out_capacity);

    if (points == NULL || point_count < 5U || out_capacity == 0U) {
        return 0U;
    }

    for (i = 2U; i + 2U < point_count; i++) {
        float m2 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i - 2U);
        float m1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i - 1U);
        float c0 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i);
        float p1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i + 1U);
        float p2 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i + 2U);
        float left_shoulder = 0.5f * (m2 + m1);
        float right_shoulder = 0.5f * (p1 + p2);
        float prominence = fminf(left_shoulder - c0, right_shoulder - c0);

        if (c0 <= m2 &&
            c0 <= m1 &&
            c0 <= p1 &&
            c0 <= p2 &&
            prominence >= DPMZM_AUTO_MIN_PROMINENCE_DB &&
            raw_count < (uint32_t)(sizeof(raw) / sizeof(raw[0]))) {
            raw[raw_count].bias_v = points[i].sweep_v;
            raw[raw_count].metric_f1_dbm = c0;
            raw[raw_count].dc_mean_v = points[i].dc_mean;
            raw[raw_count].symmetry_score = fabsf((left_shoulder - c0) -
                                                  (right_shoulder - c0));
            raw[raw_count].point_index = (uint16_t)i;
            raw[raw_count].valid = true;
            raw_count++;
        }
    }

    sort_matp_by_metric(raw, raw_count);
    for (i = 0U; i < raw_count && out_count < out_capacity; i++) {
        if (!candidate_far_enough(raw[i].bias_v, out, out_count, min_sep_v)) {
            continue;
        }
        out[out_count++] = raw[i];
    }

    if (out_count < 2U) {
        raw_count = 0U;
        memset(raw, 0, sizeof(raw));

        for (i = DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS;
             i + DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS < point_count;
             i++) {
            float m1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i - 1U);
            float c0 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i);
            float p1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP, target, points, point_count, i + 1U);

            if (c0 <= m1 &&
                c0 <= p1 &&
                raw_count < (uint32_t)(sizeof(raw) / sizeof(raw[0]))) {
                raw[raw_count].bias_v = points[i].sweep_v;
                raw[raw_count].metric_f1_dbm = c0;
                raw[raw_count].dc_mean_v = points[i].dc_mean;
                raw[raw_count].symmetry_score = fabsf(m1 - p1);
                raw[raw_count].point_index = (uint16_t)i;
                raw[raw_count].valid = true;
                raw_count++;
            }
        }

        sort_matp_by_metric(raw, raw_count);
        for (i = 0U; i < raw_count && out_count < out_capacity; i++) {
            if (!candidate_far_enough(raw[i].bias_v, out, out_count, min_sep_v)) {
                continue;
            }
            out[out_count++] = raw[i];
        }
    }

    return out_count;
}

static void build_plateau_region_from_peak(const dpmzm_scan_point_t *points,
                                           uint32_t point_count,
                                           dpmzm_scan_target_t target,
                                           uint32_t peak_idx,
                                           uint32_t left_limit,
                                           uint32_t right_limit,
                                           const dpmzm_matp_candidate_t *matp_candidates,
                                           uint32_t matp_count,
                                           float guard_v,
                                           dpmzm_sensitive_region_t *region)
{
    float peak_metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                            target,
                                            points,
                                            point_count,
                                            peak_idx);
    uint32_t j;

    memset(region, 0, sizeof(*region));
    region->left_matp_v = points[left_limit].sweep_v;
    region->right_matp_v = points[right_limit].sweep_v;
    region->plateau_peak_dbm = peak_metric;
    region->plateau_left_v = points[peak_idx].sweep_v;
    region->plateau_right_v = points[peak_idx].sweep_v;

    for (j = peak_idx; j > left_limit; j--) {
        float next_bias = points[j - 1U].sweep_v;
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           j - 1U);
        if (bias_near_any_matp(next_bias, matp_candidates, matp_count, guard_v) &&
            fabsf(next_bias - points[peak_idx].sweep_v) > 1e-6f) {
            break;
        }
        if ((peak_metric - metric) > DPMZM_AUTO_PLATEAU_DROP_DB) {
            break;
        }
        region->plateau_left_v = next_bias;
    }

    for (j = peak_idx; j < right_limit; j++) {
        float next_bias = points[j + 1U].sweep_v;
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           j + 1U);
        if (bias_near_any_matp(next_bias, matp_candidates, matp_count, guard_v) &&
            fabsf(next_bias - points[peak_idx].sweep_v) > 1e-6f) {
            break;
        }
        if ((peak_metric - metric) > DPMZM_AUTO_PLATEAU_DROP_DB) {
            break;
        }
        region->plateau_right_v = next_bias;
    }

    region->plateau_center_v = 0.5f * (region->plateau_left_v + region->plateau_right_v);
    region->plateau_width_v = region->plateau_right_v - region->plateau_left_v;
    region->valid = true;
}

static bool pick_sensitive_region_fallback(const dpmzm_scan_point_t *points,
                                           uint32_t point_count,
                                           dpmzm_scan_target_t target,
                                           const dpmzm_matp_candidate_t *matp_candidates,
                                           uint32_t matp_count,
                                           float step_v,
                                           dpmzm_sensitive_region_t *main_region,
                                           dpmzm_sensitive_region_t *backup_region)
{
    float guard_v = matp_min_separation_v(step_v);
    uint32_t left_limit;
    uint32_t right_limit;
    uint32_t best_idx = UINT32_MAX;
    uint32_t backup_idx = UINT32_MAX;
    float best_metric = -FLT_MAX;
    float backup_metric = -FLT_MAX;
    uint32_t i;

    memset(main_region, 0, sizeof(*main_region));
    memset(backup_region, 0, sizeof(*backup_region));

    if (points == NULL || point_count == 0U) {
        return false;
    }

    left_limit = (point_count > (2U * DPMZM_AUTO_EDGE_MARGIN_POINTS + 1U))
                     ? DPMZM_AUTO_EDGE_MARGIN_POINTS
                     : 0U;
    right_limit = (point_count > (2U * DPMZM_AUTO_EDGE_MARGIN_POINTS + 1U))
                      ? (point_count - 1U - DPMZM_AUTO_EDGE_MARGIN_POINTS)
                      : (point_count - 1U);
    if (right_limit <= left_limit) {
        return false;
    }

    for (i = left_limit; i <= right_limit; i++) {
        float bias_v = points[i].sweep_v;
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           i);

        if (bias_near_any_matp(bias_v, matp_candidates, matp_count, guard_v)) {
            continue;
        }

        if (metric > best_metric) {
            backup_metric = best_metric;
            backup_idx = best_idx;
            best_metric = metric;
            best_idx = i;
        } else if (metric > backup_metric &&
                   best_idx != UINT32_MAX &&
                   fabsf(bias_v - points[best_idx].sweep_v) >= guard_v) {
            backup_metric = metric;
            backup_idx = i;
        }
    }

    if (best_idx == UINT32_MAX) {
        return false;
    }

    build_plateau_region_from_peak(points,
                                   point_count,
                                   target,
                                   best_idx,
                                   left_limit,
                                   right_limit,
                                   matp_candidates,
                                   matp_count,
                                   guard_v,
                                   main_region);

    if (backup_idx != UINT32_MAX) {
        build_plateau_region_from_peak(points,
                                       point_count,
                                       target,
                                       backup_idx,
                                       left_limit,
                                       right_limit,
                                       matp_candidates,
                                       matp_count,
                                       guard_v,
                                       backup_region);
    }

    return main_region->valid;
}

static void sort_matp_anchors_by_index(dpmzm_matp_anchor_t *anchors, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < count; i++) {
        for (j = i + 1U; j < count; j++) {
            if (anchors[j].point_index < anchors[i].point_index) {
                dpmzm_matp_anchor_t tmp = anchors[i];
                anchors[i] = anchors[j];
                anchors[j] = tmp;
            }
        }
    }
}

static bool anchor_far_enough(uint32_t index,
                              const dpmzm_matp_anchor_t *anchors,
                              uint32_t count,
                              uint32_t min_sep_points)
{
    uint32_t i;

    for (i = 0U; i < count; i++) {
        uint32_t existing = anchors[i].point_index;
        uint32_t delta = (index > existing) ? (index - existing) : (existing - index);

        if (delta <= min_sep_points) {
            return false;
        }
    }

    return true;
}

static float best_smoothed_metric_dbm(const dpmzm_scan_point_t *points,
                                      uint32_t point_count,
                                      dpmzm_scan_target_t target,
                                      uint32_t left_idx,
                                      uint32_t right_idx)
{
    float best = -FLT_MAX;
    uint32_t i;

    for (i = left_idx; i <= right_idx; i++) {
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           i);
        if (metric > best) {
            best = metric;
        }
    }

    return best;
}

static bool edge_looks_like_truncated_valley(const dpmzm_scan_point_t *points,
                                             uint32_t point_count,
                                             dpmzm_scan_target_t target,
                                             uint32_t edge_idx,
                                             uint32_t inner_idx,
                                             float best_metric_dbm)
{
    float edge_metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           edge_idx);
    float inner_metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                            target,
                                            points,
                                            point_count,
                                            inner_idx);

    return (inner_metric - edge_metric) >= DPMZM_AUTO_EDGE_VALLEY_RISE_DB &&
           (best_metric_dbm - edge_metric) >= DPMZM_AUTO_EDGE_VALLEY_RISE_DB;
}

static uint32_t build_matp_anchors_with_edges(const dpmzm_scan_point_t *points,
                                              uint32_t point_count,
                                              dpmzm_scan_target_t target,
                                              const dpmzm_matp_candidate_t *matp_candidates,
                                              uint32_t matp_count,
                                              dpmzm_matp_anchor_t *anchors,
                                              uint32_t anchor_capacity)
{
    uint32_t count = 0U;
    uint32_t i;
    uint32_t left_idx;
    uint32_t right_idx;
    uint32_t inner_idx;
    float best_metric;

    if (points == NULL || matp_candidates == NULL || anchors == NULL ||
        point_count <= (2U * DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS + 1U) ||
        anchor_capacity == 0U) {
        return 0U;
    }

    memset(anchors, 0, sizeof(*anchors) * anchor_capacity);

    for (i = 0U; i < matp_count && count < anchor_capacity; i++) {
        if (!matp_candidates[i].valid ||
            matp_candidates[i].point_index >= point_count) {
            continue;
        }
        anchors[count].point_index = matp_candidates[i].point_index;
        anchors[count].bias_v = matp_candidates[i].bias_v;
        anchors[count].valid = true;
        anchors[count].virtual_edge = false;
        count++;
    }

    left_idx = DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS;
    right_idx = point_count - 1U - DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS;
    best_metric = best_smoothed_metric_dbm(points, point_count, target, left_idx, right_idx);

    inner_idx = left_idx + DPMZM_AUTO_EDGE_MARGIN_POINTS;
    if (inner_idx > right_idx) {
        inner_idx = right_idx;
    }
    if (count < anchor_capacity &&
        anchor_far_enough(left_idx, anchors, count, DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS) &&
        edge_looks_like_truncated_valley(points, point_count, target, left_idx, inner_idx, best_metric)) {
        anchors[count].point_index = left_idx;
        anchors[count].bias_v = points[left_idx].sweep_v;
        anchors[count].valid = true;
        anchors[count].virtual_edge = true;
        count++;
    }

    inner_idx = (right_idx > DPMZM_AUTO_EDGE_MARGIN_POINTS)
                    ? (right_idx - DPMZM_AUTO_EDGE_MARGIN_POINTS)
                    : left_idx;
    if (inner_idx < left_idx) {
        inner_idx = left_idx;
    }
    if (count < anchor_capacity &&
        anchor_far_enough(right_idx, anchors, count, DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS) &&
        edge_looks_like_truncated_valley(points, point_count, target, right_idx, inner_idx, best_metric)) {
        anchors[count].point_index = right_idx;
        anchors[count].bias_v = points[right_idx].sweep_v;
        anchors[count].valid = true;
        anchors[count].virtual_edge = true;
        count++;
    }

    sort_matp_anchors_by_index(anchors, count);
    return count;
}

static bool build_plateau_region_between_anchors(const dpmzm_scan_point_t *points,
                                                 uint32_t point_count,
                                                 dpmzm_scan_target_t target,
                                                 const dpmzm_matp_anchor_t *left_anchor,
                                                 const dpmzm_matp_anchor_t *right_anchor,
                                                 dpmzm_sensitive_region_t *region)
{
    uint32_t left_idx;
    uint32_t right_idx;
    uint32_t search_left;
    uint32_t search_right;
    uint32_t peak_idx;
    uint32_t j;
    float peak_metric = -FLT_MAX;

    if (points == NULL || left_anchor == NULL || right_anchor == NULL || region == NULL) {
        return false;
    }
    if (!left_anchor->valid || !right_anchor->valid) {
        return false;
    }
    if (right_anchor->point_index <= left_anchor->point_index + 1U ||
        right_anchor->point_index >= point_count) {
        return false;
    }

    left_idx = left_anchor->point_index;
    right_idx = right_anchor->point_index;
    search_left = left_idx + 1U;
    search_right = right_idx - 1U;
    peak_idx = search_left;

    for (j = search_left; j <= search_right; j++) {
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           j);
        if (metric > peak_metric) {
            peak_metric = metric;
            peak_idx = j;
        }
    }

    memset(region, 0, sizeof(*region));
    region->left_matp_v = left_anchor->bias_v;
    region->right_matp_v = right_anchor->bias_v;
    region->plateau_peak_dbm = peak_metric;
    region->plateau_left_v = points[peak_idx].sweep_v;
    region->plateau_right_v = points[peak_idx].sweep_v;

    for (j = peak_idx; j > left_idx; j--) {
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           j - 1U);
        if ((peak_metric - metric) > DPMZM_AUTO_PLATEAU_DROP_DB) {
            break;
        }
        region->plateau_left_v = points[j - 1U].sweep_v;
    }

    for (j = peak_idx; j < right_idx; j++) {
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                           target,
                                           points,
                                           point_count,
                                           j + 1U);
        if ((peak_metric - metric) > DPMZM_AUTO_PLATEAU_DROP_DB) {
            break;
        }
        region->plateau_right_v = points[j + 1U].sweep_v;
    }

    region->plateau_center_v = 0.5f * (region->plateau_left_v + region->plateau_right_v);
    region->plateau_width_v = region->plateau_right_v - region->plateau_left_v;
    region->valid = true;
    return true;
}

static bool __attribute__((unused)) pick_sensitive_regions(const dpmzm_scan_point_t *points,
                                   uint32_t point_count,
                                   dpmzm_matp_candidate_t *matp_candidates,
                                   uint32_t matp_count,
                                   dpmzm_scan_target_t target,
                                   float step_v,
                                   dpmzm_sensitive_region_t *main_region,
                                   dpmzm_sensitive_region_t *backup_region)
{
    dpmzm_matp_anchor_t anchors[DPMZM_AUTO_MATP_CANDIDATES_MAX + 2U];
    uint32_t anchor_count;
    dpmzm_sensitive_region_t best = {0};
    dpmzm_sensitive_region_t second = {0};
    uint32_t i;

    memset(main_region, 0, sizeof(*main_region));
    memset(backup_region, 0, sizeof(*backup_region));

    if (points == NULL || matp_candidates == NULL || matp_count == 0U) {
        return false;
    }
    anchor_count = build_matp_anchors_with_edges(points,
                                                 point_count,
                                                 target,
                                                 matp_candidates,
                                                 matp_count,
                                                 anchors,
                                                 (uint32_t)(sizeof(anchors) / sizeof(anchors[0])));
    if (anchor_count < 2U) {
        return pick_sensitive_region_fallback(points,
                                              point_count,
                                              target,
                                              matp_candidates,
                                              matp_count,
                                              step_v,
                                              main_region,
                                              backup_region);
    }

    for (i = 0U; i + 1U < anchor_count; i++) {
        dpmzm_sensitive_region_t region;

        if (!build_plateau_region_between_anchors(points,
                                                  point_count,
                                                  target,
                                                  &anchors[i],
                                                  &anchors[i + 1U],
                                                  &region)) {
            continue;
        }

        if (!best.valid || region.plateau_peak_dbm > best.plateau_peak_dbm) {
            second = best;
            best = region;
        } else if (!second.valid || region.plateau_peak_dbm > second.plateau_peak_dbm) {
            second = region;
        }
    }

    if (!best.valid) {
        return pick_sensitive_region_fallback(points,
                                              point_count,
                                              target,
                                              matp_candidates,
                                              matp_count,
                                              step_v,
                                              main_region,
                                              backup_region);
    }

    *main_region = best;
    *backup_region = second;
    return true;
}

static bool pick_matp_high_power_platform(const dpmzm_scan_point_t *points,
                                          uint32_t point_count,
                                          dpmzm_scan_target_t target,
                                          float *center_v_out)
{
    uint32_t i;
    float p_min = FLT_MAX;
    float p_max = -FLT_MAX;
    float span;
    float best_score = -FLT_MAX;
    float best_center = 0.0f;
    float best_left = 0.0f;
    float best_right = 0.0f;
    float best_mean = 0.0f;
    float best_width = 0.0f;
    uint32_t best_count = 0U;
    bool best_valid = false;

    if (points == NULL || point_count == 0U || center_v_out == NULL) {
        return false;
    }

    for (i = 0U; i < point_count; i++) {
        float metric = metric_from_point(DPMZM_SCAN_STAGE_MATP, target, &points[i]);
        float power = metric * metric;

        if (power < p_min) {
            p_min = power;
        }
        if (power > p_max) {
            p_max = power;
        }
    }

    span = p_max - p_min;
    if (span <= fmaxf(p_max, 1.0f) * 1e-12f) {
        float best_metric = -FLT_MAX;
        uint32_t best_index = 0U;

        for (i = 0U; i < point_count; i++) {
            float metric = metric_from_point(DPMZM_SCAN_STAGE_MATP, target, &points[i]);
            if (metric > best_metric) {
                best_metric = metric;
                best_index = i;
            }
        }
        *center_v_out = points[best_index].sweep_v;
        printf("[dpmzm][auto] %s MATP platform fallback: flat power, use max %+.3fV\r\n",
               dpmzm_scan_target_name(target),
               (double)*center_v_out);
        return true;
    }

    i = 0U;
    while (i < point_count) {
        uint32_t left = i;
        uint32_t right;
        uint32_t count;
        float width_v;
        float weighted_sum = 0.0f;
        float weight_sum = 0.0f;
        float mean_norm = 0.0f;
        float width_norm;
        float score;
        uint32_t j;

        float metric = metric_from_point(DPMZM_SCAN_STAGE_MATP, target, &points[i]);
        float power = metric * metric;
        float pnorm = (power - p_min) / span;
        if (pnorm < DPMZM_AUTO_MATP_PLATFORM_THRESHOLD) {
            i++;
            continue;
        }

        right = i;
        while (right + 1U < point_count) {
            float next_metric = metric_from_point(DPMZM_SCAN_STAGE_MATP,
                                                  target,
                                                  &points[right + 1U]);
            float next_power = next_metric * next_metric;
            float next_norm = (next_power - p_min) / span;
            if (next_norm < DPMZM_AUTO_MATP_PLATFORM_THRESHOLD) {
                break;
            }
            right++;
        }

        count = right - left + 1U;
        width_v = fabsf(points[right].sweep_v - points[left].sweep_v);
        if (count >= DPMZM_AUTO_MATP_PLATFORM_MIN_POINTS &&
            width_v >= DPMZM_AUTO_MATP_PLATFORM_MIN_WIDTH_V) {
            for (j = left; j <= right; j++) {
                float row_metric = metric_from_point(DPMZM_SCAN_STAGE_MATP,
                                                     target,
                                                     &points[j]);
                float row_power = row_metric * row_metric;
                float row_norm = (row_power - p_min) / span;
                float weight = fmaxf(row_norm, 1e-9f);
                weighted_sum += points[j].sweep_v * weight;
                weight_sum += weight;
                mean_norm += row_norm;
            }

            mean_norm /= (float)count;
            width_norm = fminf(width_v / DPMZM_AUTO_MATP_PLATFORM_WIDTH_REF_V, 1.0f);
            score = mean_norm + (DPMZM_AUTO_MATP_PLATFORM_WIDTH_WEIGHT * width_norm);
            if (score > best_score) {
                best_score = score;
                best_center = weighted_sum / weight_sum;
                best_left = points[left].sweep_v;
                best_right = points[right].sweep_v;
                best_mean = mean_norm;
                best_width = width_v;
                best_count = count;
                best_valid = true;
            }
        }

        i = right + 1U;
    }

    if (!best_valid) {
        float best_metric = -FLT_MAX;
        uint32_t best_index = 0U;

        for (i = 0U; i < point_count; i++) {
            float metric = metric_from_point(DPMZM_SCAN_STAGE_MATP, target, &points[i]);
            if (metric > best_metric) {
                best_metric = metric;
                best_index = i;
            }
        }
        *center_v_out = points[best_index].sweep_v;
        printf("[dpmzm][auto] %s MATP platform fallback: no wide high-power platform, use max %+.3fV\r\n",
               dpmzm_scan_target_name(target),
               (double)*center_v_out);
        return true;
    }

    *center_v_out = best_center;
    printf("[dpmzm][auto] %s MATP platform: %+.3f..%+.3fV center=%+.3fV score=%.3f mean=%.3f width=%.3fV points=%lu\r\n",
           dpmzm_scan_target_name(target),
           (double)best_left,
           (double)best_right,
           (double)best_center,
           (double)best_score,
           (double)best_mean,
           (double)best_width,
           (unsigned long)best_count);
    return true;
}

static bool qtp_curve_usable(const dpmzm_scan_point_t *points, uint32_t point_count)
{
    float min_metric = FLT_MAX;
    float max_metric = -FLT_MAX;
    uint32_t i;
    uint32_t usable_count = 0U;

    if (points == NULL || point_count <= (2U * DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS)) {
        return false;
    }

    for (i = DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS;
         i + DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS < point_count;
         i++) {
        float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_QTP,
                                           DPMZM_SCAN_TARGET_P,
                                           points,
                                           point_count,
                                           i);
        if (metric < min_metric) {
            min_metric = metric;
        }
        if (metric > max_metric) {
            max_metric = metric;
        }
        usable_count++;
    }

    return usable_count >= 3U &&
           (max_metric - min_metric) >= DPMZM_AUTO_MIN_QTP_RANGE_DB;
}

static bool qtp_candidate_score_dbm(const dpmzm_scan_point_t *points,
                                    uint32_t point_count,
                                    uint32_t index,
                                    float *score_dbm)
{
    float prev_metric;
    float metric;
    float next_metric;

    if (points == NULL || score_dbm == NULL ||
        !scan_candidate_index_allowed(point_count, index)) {
        return false;
    }

    prev_metric = metric_dbm_from_point(DPMZM_SCAN_STAGE_QTP,
                                        DPMZM_SCAN_TARGET_P,
                                        &points[index - 1U]);
    metric = metric_dbm_from_point(DPMZM_SCAN_STAGE_QTP,
                                   DPMZM_SCAN_TARGET_P,
                                   &points[index]);
    next_metric = metric_dbm_from_point(DPMZM_SCAN_STAGE_QTP,
                                        DPMZM_SCAN_TARGET_P,
                                        &points[index + 1U]);

    /*
     * A true QTP valley should be supported by neighbouring points. A single
     * bin that drops far below both neighbours is usually an ADC/settle/noise
     * dropout and must not steal the automatic P-branch decision.
     */
    if ((prev_metric - metric) >= DPMZM_AUTO_QTP_SPIKE_REJECT_DB &&
        (next_metric - metric) >= DPMZM_AUTO_QTP_SPIKE_REJECT_DB) {
        return false;
    }

    *score_dbm = (prev_metric + metric + next_metric) / 3.0f;
    return true;
}

static bool pick_qtp_best_point_scored(const dpmzm_scan_point_t *points,
                                       uint32_t point_count,
                                       float *best_p_v,
                                       float *best_score_dbm,
                                       uint32_t *best_index_out)
{
    float best_metric = FLT_MAX;
    float best_value = 0.0f;
    uint32_t best_index = 0U;
    float best_positive_metric = FLT_MAX;
    float best_positive_value = 0.0f;
    uint32_t best_positive_index = 0U;
    uint32_t i;

    if (points == NULL || point_count == 0U || best_p_v == NULL) {
        return false;
    }

    for (i = 0U; i < point_count; i++) {
        float metric = 0.0f;

        if (!qtp_candidate_score_dbm(points, point_count, i, &metric)) {
            continue;
        }
        if (metric < best_metric) {
            best_metric = metric;
            best_value = points[i].sweep_v;
            best_index = i;
        }
        if (points[i].sweep_v >= 0.0f && metric < best_positive_metric) {
            best_positive_metric = metric;
            best_positive_value = points[i].sweep_v;
            best_positive_index = i;
        }
    }

    if (best_metric == FLT_MAX) {
        return false;
    }

    /*
     * The positive-branch workflow should not jump to the negative P-QTP branch
     * just because two nearly equivalent QTP valleys differ by a fraction of a
     * dB in one coarse scan. Prefer the best positive-P valley when it is close
     * enough to the global best; still allow the negative branch if the positive
     * valley is clearly worse.
     */
    if (best_positive_metric != FLT_MAX &&
        best_positive_metric <= (best_metric + DPMZM_AUTO_QTP_POSITIVE_MARGIN_DB)) {
        best_metric = best_positive_metric;
        best_value = best_positive_value;
        best_index = best_positive_index;
    }

    *best_p_v = best_value;
    if (best_score_dbm != NULL) {
        *best_score_dbm = best_metric;
    }
    if (best_index_out != NULL) {
        *best_index_out = best_index;
    }
    return true;
}

static bool pick_qtp_best_point(const dpmzm_scan_point_t *points,
                                uint32_t point_count,
                                float *best_p_v)
{
    return pick_qtp_best_point_scored(points,
                                      point_count,
                                      best_p_v,
                                      NULL,
                                      NULL);
}

static uint32_t extract_mitp_candidates(const dpmzm_scan_point_t *points,
                                        uint32_t point_count,
                                        dpmzm_scan_target_t target,
                                        float step_v,
                                        dpmzm_mitp_candidate_t *out,
                                        uint32_t out_capacity)
{
    dpmzm_mitp_candidate_t raw[DPMZM_AUTO_MITP_CANDIDATES_MAX * 4U];
    uint32_t raw_count = 0U;
    uint32_t out_count = 0U;
    uint32_t i;
    float min_sep_v = matp_min_separation_v(step_v);

    memset(raw, 0, sizeof(raw));
    memset(out, 0, sizeof(*out) * out_capacity);

    if (points == NULL || point_count < 5U || out_capacity == 0U) {
        return 0U;
    }

    for (i = 2U; i + 2U < point_count; i++) {
        float m2 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i - 2U);
        float m1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i - 1U);
        float c0 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i);
        float p1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i + 1U);
        float p2 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i + 2U);
        float left_shoulder = 0.5f * (m2 + m1);
        float right_shoulder = 0.5f * (p1 + p2);
        float prominence = fminf(left_shoulder - c0, right_shoulder - c0);

        if (c0 <= m2 &&
            c0 <= m1 &&
            c0 <= p1 &&
            c0 <= p2 &&
            prominence >= DPMZM_AUTO_MIN_PROMINENCE_DB &&
            raw_count < (uint32_t)(sizeof(raw) / sizeof(raw[0]))) {
            raw[raw_count].bias_v = points[i].sweep_v;
            raw[raw_count].metric_f1_dbm = c0;
            raw[raw_count].dc_mean_v = points[i].dc_mean;
            raw[raw_count].point_index = (uint16_t)i;
            raw[raw_count].valid = true;
            raw_count++;
        }
    }

    sort_mitp_by_metric(raw, raw_count);
    for (i = 0U; i < raw_count && out_count < out_capacity; i++) {
        uint32_t j;
        bool far_enough = true;

        for (j = 0U; j < out_count; j++) {
            if (fabsf(out[j].bias_v - raw[i].bias_v) < min_sep_v) {
                far_enough = false;
                break;
            }
        }
        if (!far_enough) {
            continue;
        }
        out[out_count++] = raw[i];
    }

    if (out_count < 2U) {
        raw_count = 0U;
        memset(raw, 0, sizeof(raw));

        for (i = DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS;
             i + DPMZM_AUTO_SCAN_EDGE_REJECT_POINTS < point_count;
             i++) {
            float m1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i - 1U);
            float c0 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i);
            float p1 = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MITP, target, points, point_count, i + 1U);

            if (c0 <= m1 &&
                c0 <= p1 &&
                raw_count < (uint32_t)(sizeof(raw) / sizeof(raw[0]))) {
                raw[raw_count].bias_v = points[i].sweep_v;
                raw[raw_count].metric_f1_dbm = c0;
                raw[raw_count].dc_mean_v = points[i].dc_mean;
                raw[raw_count].point_index = (uint16_t)i;
                raw[raw_count].valid = true;
                raw_count++;
            }
        }

        sort_mitp_by_metric(raw, raw_count);
        for (i = 0U; i < raw_count && out_count < out_capacity; i++) {
            uint32_t j;
            bool far_enough = true;

            for (j = 0U; j < out_count; j++) {
                if (fabsf(out[j].bias_v - raw[i].bias_v) < min_sep_v) {
                    far_enough = false;
                    break;
                }
            }
            if (!far_enough) {
                continue;
            }
            out[out_count++] = raw[i];
        }
    }

    return out_count;
}

static bool pick_mitp_branch(const dpmzm_scan_point_t *points,
                             uint32_t point_count,
                             dpmzm_scan_target_t target,
                             float step_v,
                             float *best_bias_v)
{
    dpmzm_mitp_candidate_t candidates[DPMZM_AUTO_MITP_CANDIDATES_MAX];
    uint32_t candidate_count;
    uint32_t selected_index = 0U;
    float selected_dc = FLT_MAX;
    float selected_raw_dbm = FLT_MAX;
    uint32_t i;

    if (best_bias_v == NULL) {
        return false;
    }

    candidate_count = extract_mitp_candidates(points,
                                              point_count,
                                              target,
                                              step_v,
                                              candidates,
                                              DPMZM_AUTO_MITP_CANDIDATES_MAX);
    if (candidate_count == 0U) {
        return false;
    }

    printf("[dpmzm][auto] %s-MITP candidates:",
           dpmzm_scan_target_name(target));
    for (i = 0U; i < candidate_count; i++) {
        float raw_dbm = metric_dbm_from_point(DPMZM_SCAN_STAGE_MITP,
                                              target,
                                              &points[candidates[i].point_index]);
        printf(" %+.3fV(raw=%.2fdBm,smooth=%.2fdBm,dc=%.6f)",
               (double)candidates[i].bias_v,
               (double)raw_dbm,
               (double)candidates[i].metric_f1_dbm,
               (double)candidates[i].dc_mean_v);

        /* Once a point is a real MITP valley, DC decides the branch. RF depth
         * only breaks near-ties so a shallow-but-valid low-DC branch is kept. */
        if (candidates[i].dc_mean_v < selected_dc ||
            (fabsf(candidates[i].dc_mean_v - selected_dc) < 1e-6f &&
             raw_dbm < selected_raw_dbm)) {
            selected_index = i;
            selected_dc = candidates[i].dc_mean_v;
            selected_raw_dbm = raw_dbm;
        }
    }

    printf(" -> select %+.3fV\r\n",
           (double)candidates[selected_index].bias_v);

    candidates[selected_index].selected_as_mitp = true;
    *best_bias_v = candidates[selected_index].bias_v;
    return true;
}

static bool run_collect_scan(dpmzm_scan_request_t *scan_req,
                             dpmzm_scan_stage_t stage,
                             dpmzm_scan_target_t target,
                             float start_v,
                             float stop_v,
                             float step_v,
                             dpmzm_scan_summary_t *summary_out,
                             uint32_t *point_count_out)
{
    scan_req->stage = stage;
    scan_req->target = target;
    scan_req->start_v = start_v;
    scan_req->stop_v = stop_v;
    scan_req->step_v = step_v;

    return dpmzm_scan_run_collect(scan_req,
                                  summary_out,
                                  s_scan_points,
                                  DPMZM_AUTO_SCAN_POINTS_MAX,
                                  point_count_out);
}

static float clamp_bias(float value, float min_v, float max_v)
{
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

static void build_scan_window(float center_v,
                              float range_v,
                              float min_v,
                              float max_v,
                              float *start_v,
                              float *stop_v)
{
    if (start_v == NULL || stop_v == NULL) {
        return;
    }

    *start_v = clamp_bias(center_v - fabsf(range_v), min_v, max_v);
    *stop_v = clamp_bias(center_v + fabsf(range_v), min_v, max_v);
}

static bool pick_best_point(dpmzm_scan_stage_t stage,
                            dpmzm_scan_target_t target,
                            const dpmzm_scan_point_t *points,
                            uint32_t point_count,
                            float *best_bias_v,
                            float *best_metric_dbm,
                            uint32_t *best_index_out)
{
    float best_metric = FLT_MAX;
    float best_bias = 0.0f;
    uint32_t best_index = 0U;
    uint32_t i;

    if (points == NULL || point_count == 0U || best_bias_v == NULL) {
        return false;
    }

    if (stage == DPMZM_SCAN_STAGE_QTP && target == DPMZM_SCAN_TARGET_P) {
        return pick_qtp_best_point_scored(points,
                                          point_count,
                                          best_bias_v,
                                          best_metric_dbm,
                                          best_index_out);
    }

    for (i = 0U; i < point_count; i++) {
        float metric = metric_dbm_from_point(stage, target, &points[i]);
        if (!scan_candidate_index_allowed(point_count, i)) {
            continue;
        }
        if (metric < best_metric) {
            best_metric = metric;
            best_bias = points[i].sweep_v;
            best_index = i;
        }
    }

    if (best_metric == FLT_MAX) {
        return false;
    }

    *best_bias_v = best_bias;
    if (best_metric_dbm != NULL) {
        *best_metric_dbm = best_metric;
    }
    if (best_index_out != NULL) {
        *best_index_out = best_index;
    }
    return true;
}

static bool best_point_near_edge(const dpmzm_scan_point_t *points,
                                 uint32_t point_count,
                                 uint32_t best_index,
                                 float step_v)
{
    float edge_margin;
    float best_v;
    float left_v;
    float right_v;

    if (points == NULL || point_count == 0U || best_index >= point_count) {
        return false;
    }

    edge_margin = fmaxf(2.0f * fabsf(step_v), 0.05f);
    best_v = points[best_index].sweep_v;
    left_v = fminf(points[0].sweep_v, points[point_count - 1U].sweep_v);
    right_v = fmaxf(points[0].sweep_v, points[point_count - 1U].sweep_v);

    return (fabsf(best_v - left_v) <= edge_margin) ||
           (fabsf(best_v - right_v) <= edge_margin);
}

static bool apply_fixed_biases(dpmzm_scan_request_t *scan_req,
                               float vi,
                               float vq,
                               float vp)
{
    if (scan_req == NULL) {
        return false;
    }

    scan_req->bias_i_v = vi;
    scan_req->bias_q_v = vq;
    scan_req->bias_p_v = vp;
    if (scan_req->bias_apply_fn != NULL) {
        return scan_req->bias_apply_fn(vi, vq, vp) == 0;
    }
    return true;
}

static bool run_best_window_scan(dpmzm_scan_request_t *scan_req,
                                 dpmzm_scan_stage_t stage,
                                 dpmzm_scan_target_t target,
                                 float center_v,
                                 float range_v,
                                 float step_v,
                                 float min_v,
                                 float max_v,
                                 float *best_v,
                                 bool *edge_out)
{
    dpmzm_scan_summary_t summary;
    float start_v = 0.0f;
    float stop_v = 0.0f;
    uint32_t point_count = 0U;
    uint32_t best_index = 0U;
    float best_metric_dbm = 0.0f;

    if (scan_req == NULL || best_v == NULL) {
        return false;
    }

    build_scan_window(center_v, range_v, min_v, max_v, &start_v, &stop_v);
    if (!run_collect_scan(scan_req,
                          stage,
                          target,
                          start_v,
                          stop_v,
                          step_v,
                          &summary,
                          &point_count)) {
        return false;
    }

    if (!pick_best_point(stage,
                         target,
                         s_scan_points,
                         point_count,
                         best_v,
                         &best_metric_dbm,
                         &best_index)) {
        return false;
    }

    if (edge_out != NULL) {
        *edge_out = best_point_near_edge(s_scan_points,
                                         point_count,
                                         best_index,
                                         step_v);
    }

    printf("[dpmzm][auto] fine pick: stage=%s target=%s center=%+.3fV range=%.3f step=%.3f best=%+.3fV %.2fdBm edge=%s\r\n",
           dpmzm_scan_stage_name(stage),
           dpmzm_scan_target_name(target),
           (double)center_v,
           (double)range_v,
           (double)step_v,
           (double)*best_v,
           (double)best_metric_dbm,
           (edge_out != NULL && *edge_out) ? "yes" : "no");
    return true;
}

static bool run_turning_point_scan(dpmzm_scan_request_t *scan_req,
                                   dpmzm_scan_stage_t stage,
                                   dpmzm_scan_target_t target,
                                   float center_v,
                                   float fallback_window_v,
                                   float final_window_v,
                                   float final_step_v,
                                   float turn_step_v,
                                   int max_shifts,
                                   float min_v,
                                   float max_v,
                                   float *best_v,
                                   bool *edge_rescan_out)
{
    float center;
    float bracket_center = 0.0f;
    bool bracketed = false;
    uint32_t attempt = 0U;
    uint32_t hard_limit;

    if (scan_req == NULL || best_v == NULL ||
        turn_step_v <= 0.0f || final_step_v <= 0.0f) {
        return false;
    }

    center = clamp_bias(center_v, min_v, max_v);
    hard_limit = (uint32_t)ceilf((max_v - min_v) / turn_step_v) + 4U;
    if (max_shifts >= 0 && (uint32_t)max_shifts < hard_limit) {
        hard_limit = (uint32_t)max_shifts + 1U;
    }

    while (attempt < hard_limit) {
        dpmzm_scan_summary_t summary;
        uint32_t point_count = 0U;
        float probe_center = center;
        float start_v;
        float stop_v;
        float left_m;
        float mid_m;
        float right_m;
        uint32_t mid_index;
        uint32_t best_index;
        float next_center;

        if (probe_center - turn_step_v < min_v) {
            probe_center = min_v + turn_step_v;
        }
        if (probe_center + turn_step_v > max_v) {
            probe_center = max_v - turn_step_v;
        }
        probe_center = clamp_bias(probe_center, min_v, max_v);

        start_v = clamp_bias(probe_center - turn_step_v, min_v, max_v);
        stop_v = clamp_bias(probe_center + turn_step_v, min_v, max_v);
        if (!run_collect_scan(scan_req,
                              stage,
                              target,
                              start_v,
                              stop_v,
                              turn_step_v,
                              &summary,
                              &point_count)) {
            return false;
        }
        if (point_count < 3U) {
            break;
        }

        mid_index = 0U;
        {
            float best_delta = FLT_MAX;
            uint32_t i;
            for (i = 0U; i < point_count; i++) {
                float delta = fabsf(s_scan_points[i].sweep_v - probe_center);
                if (delta < best_delta) {
                    best_delta = delta;
                    mid_index = i;
                }
            }
        }
        if (mid_index == 0U || mid_index + 1U >= point_count) {
            break;
        }

        left_m = metric_from_point(stage, target, &s_scan_points[mid_index - 1U]);
        mid_m = metric_from_point(stage, target, &s_scan_points[mid_index]);
        right_m = metric_from_point(stage, target, &s_scan_points[mid_index + 1U]);

        printf("[dpmzm][auto] turn probe: stage=%s target=%s center=%+.3fV L=%.9f C=%.9f R=%.9f\r\n",
               dpmzm_scan_stage_name(stage),
               dpmzm_scan_target_name(target),
               (double)probe_center,
               (double)left_m,
               (double)mid_m,
               (double)right_m);

        if (mid_m <= left_m && mid_m <= right_m) {
            bracket_center = s_scan_points[mid_index].sweep_v;
            bracketed = true;
            printf("[dpmzm][auto] turn bracketed: stage=%s target=%s center=%+.3fV\r\n",
                   dpmzm_scan_stage_name(stage),
                   dpmzm_scan_target_name(target),
                   (double)bracket_center);
            break;
        }

        best_index = mid_index;
        if (left_m < metric_from_point(stage, target, &s_scan_points[best_index])) {
            best_index = mid_index - 1U;
        }
        if (right_m < metric_from_point(stage, target, &s_scan_points[best_index])) {
            best_index = mid_index + 1U;
        }

        next_center = clamp_bias(s_scan_points[best_index].sweep_v, min_v, max_v);
        printf("[dpmzm][auto] turn move: stage=%s target=%s %+.3fV -> %+.3fV\r\n",
               dpmzm_scan_stage_name(stage),
               dpmzm_scan_target_name(target),
               (double)center,
               (double)next_center);

        if (fabsf(next_center - center) < 1e-6f ||
            next_center <= min_v + 1e-6f ||
            next_center >= max_v - 1e-6f) {
            center = next_center;
            break;
        }

        center = next_center;
        attempt++;
    }

    if (!bracketed) {
        bool edge = false;

        printf("[dpmzm][auto] turn fallback: stage=%s target=%s center=%+.3fV range=%.3fV\r\n",
               dpmzm_scan_stage_name(stage),
               dpmzm_scan_target_name(target),
               (double)center,
               (double)fallback_window_v);
        return run_best_window_scan(scan_req,
                                    stage,
                                    target,
                                    center,
                                    fallback_window_v,
                                    final_step_v,
                                    min_v,
                                    max_v,
                                    best_v,
                                    edge_rescan_out != NULL ? edge_rescan_out : &edge);
    }

    if (!run_best_window_scan(scan_req,
                              stage,
                              target,
                              bracket_center,
                              final_window_v,
                              final_step_v,
                              min_v,
                              max_v,
                              best_v,
                              edge_rescan_out)) {
        return false;
    }

    if (edge_rescan_out != NULL && *edge_rescan_out) {
        bool second_edge = false;
        float rescanned_best = *best_v;

        printf("[dpmzm][auto] turn fine edge rescan: stage=%s target=%s around=%+.3fV\r\n",
               dpmzm_scan_stage_name(stage),
               dpmzm_scan_target_name(target),
               (double)rescanned_best);
        if (!run_best_window_scan(scan_req,
                                  stage,
                                  target,
                                  rescanned_best,
                                  final_window_v,
                                  final_step_v,
                                  min_v,
                                  max_v,
                                  best_v,
                                  &second_edge)) {
            return false;
        }
        *edge_rescan_out = true;
    }

    return true;
}

static void print_matp_candidates(const char *label,
                                  const dpmzm_matp_candidate_t *candidates,
                                  uint32_t count)
{
    uint32_t i;

    printf("[dpmzm][auto] %s MATP candidates:", label);
    if (count == 0U) {
        printf(" none\r\n");
        return;
    }

    for (i = 0U; i < count; i++) {
        printf(" %+.3fV", (double)candidates[i].bias_v);
    }
    printf("\r\n");
}

void dpmzm_auto_init(void)
{
    clear_context();
}

const dpmzm_auto_context_t *dpmzm_auto_get_context(void)
{
    return &s_auto_ctx;
}

bool dpmzm_auto_run_coarse(const dpmzm_auto_coarse_request_t *req,
                           dpmzm_auto_coarse_result_t *out)
{
    dpmzm_scan_request_t scan_req;
    dpmzm_scan_summary_t summary;
    dpmzm_auto_coarse_result_t result;
    dpmzm_matp_candidate_t i_matp[DPMZM_AUTO_MATP_CANDIDATES_MAX];
    dpmzm_matp_candidate_t q_matp[DPMZM_AUTO_MATP_CANDIDATES_MAX];
    uint32_t point_count = 0U;
    uint32_t i_matp_count = 0U;
    uint32_t q_matp_count = 0U;

    if (!request_valid(req) || out == NULL) {
        return false;
    }

    clear_context();
    s_auto_ctx.last_request = *req;
    reset_result(&result);
    memset(&scan_req, 0, sizeof(scan_req));
    memset(i_matp, 0, sizeof(i_matp));
    memset(q_matp, 0, sizeof(q_matp));
    scan_req = req->scan_template;

    set_state(DPMZM_AUTO_SCAN_I_MATP);
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MATP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_MATP,
                          DPMZM_SCAN_TARGET_I,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    i_matp_count = extract_matp_candidates(s_scan_points,
                                           point_count,
                                           DPMZM_SCAN_TARGET_I,
                                           req->sweep_step_v,
                                           i_matp,
                                           DPMZM_AUTO_MATP_CANDIDATES_MAX);
    print_matp_candidates("I", i_matp, i_matp_count);
    set_state(DPMZM_AUTO_PICK_I_PLATEAU);
    if (!pick_matp_high_power_platform(s_scan_points,
                                       point_count,
                                       DPMZM_SCAN_TARGET_I,
                                       &result.i_pqtp_init_v)) {
        result.error = DPMZM_AUTO_ERR_I_PLATEAU_NOT_FOUND;
        goto fail;
    }
    result.i_matp_ref_v = (i_matp_count > 0U) ? i_matp[0].bias_v : result.i_pqtp_init_v;
    printf("[dpmzm][auto] I plateau center: %+.3fV\r\n",
           (double)result.i_pqtp_init_v);

    set_state(DPMZM_AUTO_SCAN_Q_MATP);
    scan_req.bias_i_v = result.i_pqtp_init_v;
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MATP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_MATP,
                          DPMZM_SCAN_TARGET_Q,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    q_matp_count = extract_matp_candidates(s_scan_points,
                                           point_count,
                                           DPMZM_SCAN_TARGET_Q,
                                           req->sweep_step_v,
                                           q_matp,
                                           DPMZM_AUTO_MATP_CANDIDATES_MAX);
    print_matp_candidates("Q", q_matp, q_matp_count);
    set_state(DPMZM_AUTO_PICK_Q_PLATEAU);
    if (!pick_matp_high_power_platform(s_scan_points,
                                       point_count,
                                       DPMZM_SCAN_TARGET_Q,
                                       &result.q_pqtp_init_v)) {
        result.error = DPMZM_AUTO_ERR_Q_PLATEAU_NOT_FOUND;
        goto fail;
    }
    result.q_matp_ref_v = (q_matp_count > 0U) ? q_matp[0].bias_v : result.q_pqtp_init_v;
    printf("[dpmzm][auto] Q plateau center: %+.3fV\r\n",
           (double)result.q_pqtp_init_v);

    set_state(DPMZM_AUTO_SCAN_P_QTP);
    scan_req.bias_i_v = result.i_pqtp_init_v;
    scan_req.bias_q_v = result.q_pqtp_init_v;
    scan_req.bias_p_v = req->scan_template.bias_p_v;
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_QTP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_QTP,
                          DPMZM_SCAN_TARGET_P,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    if (!qtp_curve_usable(s_scan_points, point_count) ||
        !pick_qtp_best_point(s_scan_points, point_count, &result.p_qtp_coarse_v)) {
        result.error = DPMZM_AUTO_ERR_P_QTP_INVALID;
        goto fail;
    }
    result.p_qtp_valid = true;
    printf("[dpmzm][auto] P-QTP best: %+.3fV\r\n",
           (double)result.p_qtp_coarse_v);

    set_state(DPMZM_AUTO_SCAN_I_MITP);
    scan_req.bias_i_v = result.i_pqtp_init_v;
    scan_req.bias_q_v = result.q_pqtp_init_v;
    scan_req.bias_p_v = result.p_qtp_coarse_v;
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_MITP,
                          DPMZM_SCAN_TARGET_I,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    if (!pick_mitp_branch(s_scan_points,
                          point_count,
                          DPMZM_SCAN_TARGET_I,
                          req->sweep_step_v,
                          &result.i_mitp_coarse_v)) {
        result.error = DPMZM_AUTO_ERR_I_MITP_NOT_FOUND;
        goto fail;
    }
    result.i_mitp_valid = true;
    printf("[dpmzm][auto] I-MITP best: %+.3fV\r\n",
           (double)result.i_mitp_coarse_v);

    set_state(DPMZM_AUTO_SCAN_Q_MITP);
    scan_req.bias_i_v = result.i_mitp_coarse_v;
    scan_req.bias_q_v = result.q_pqtp_init_v;
    scan_req.bias_p_v = result.p_qtp_coarse_v;
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_MITP,
                          DPMZM_SCAN_TARGET_Q,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    if (!pick_mitp_branch(s_scan_points,
                          point_count,
                          DPMZM_SCAN_TARGET_Q,
                          req->sweep_step_v,
                          &result.q_mitp_coarse_v)) {
        result.error = DPMZM_AUTO_ERR_Q_MITP_NOT_FOUND;
        goto fail;
    }
    result.q_mitp_valid = true;
    printf("[dpmzm][auto] Q-MITP best: %+.3fV\r\n",
           (double)result.q_mitp_coarse_v);

    /*
     * Match the verified script flow: after I/Q have moved to their MITP
     * branches, run one more full P-QTP pass. P sensitivity and branch shape
     * can change noticeably after I/Q are no longer at the MATP seed plateau.
     */
    set_state(DPMZM_AUTO_SCAN_P_QTP);
    scan_req.bias_i_v = result.i_mitp_coarse_v;
    scan_req.bias_q_v = result.q_mitp_coarse_v;
    scan_req.bias_p_v = result.p_qtp_coarse_v;
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_QTP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_collect_scan(&scan_req,
                          DPMZM_SCAN_STAGE_QTP,
                          DPMZM_SCAN_TARGET_P,
                          req->sweep_min_v,
                          req->sweep_max_v,
                          req->sweep_step_v,
                          &summary,
                          &point_count)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    if (!qtp_curve_usable(s_scan_points, point_count) ||
        !pick_qtp_best_point(s_scan_points, point_count, &result.p_qtp_coarse_v)) {
        result.error = DPMZM_AUTO_ERR_P_QTP_INVALID;
        goto fail;
    }
    printf("[dpmzm][auto] P-QTP best after MITP: %+.3fV\r\n",
           (double)result.p_qtp_coarse_v);

    set_state(DPMZM_AUTO_DONE);
    result.error = DPMZM_AUTO_OK;
    s_auto_ctx.error = DPMZM_AUTO_OK;
    s_auto_ctx.last_result = result;
    s_auto_ctx.has_result = true;
    *out = result;
    return true;

fail:
    set_state(DPMZM_AUTO_FAILED);
    s_auto_ctx.error = result.error;
    s_auto_ctx.last_result = result;
    s_auto_ctx.has_result = true;
    *out = result;
    return false;
}

bool dpmzm_auto_run_fine(const dpmzm_auto_fine_request_t *req,
                         dpmzm_auto_fine_result_t *out)
{
    dpmzm_scan_request_t scan_req;
    dpmzm_auto_fine_result_t result;
    float i_final = 0.0f;
    float q_final = 0.0f;
    float p_final = 0.0f;
    bool edge = false;

    if (out == NULL) {
        return false;
    }

    reset_fine_result(&result);
    if (req == NULL) {
        result.error = DPMZM_AUTO_ERR_BAD_ARG;
        *out = result;
        return false;
    }
    if (!req->coarse_result.p_qtp_valid ||
        !req->coarse_result.i_mitp_valid ||
        !req->coarse_result.q_mitp_valid ||
        req->coarse_result.error != DPMZM_AUTO_OK) {
        result.error = DPMZM_AUTO_ERR_NO_COARSE_RESULT;
        s_auto_ctx.error = result.error;
        s_auto_ctx.last_fine_result = result;
        s_auto_ctx.has_fine_result = true;
        *out = result;
        return false;
    }
    if (!fine_request_valid(req)) {
        result.error = DPMZM_AUTO_ERR_BAD_ARG;
        s_auto_ctx.error = result.error;
        s_auto_ctx.last_fine_result = result;
        s_auto_ctx.has_fine_result = true;
        *out = result;
        return false;
    }

    s_auto_ctx.error = DPMZM_AUTO_OK;
    s_auto_ctx.has_fine_result = false;
    memset(&scan_req, 0, sizeof(scan_req));
    scan_req = req->scan_template;

    i_final = req->coarse_result.i_mitp_coarse_v;
    q_final = req->coarse_result.q_mitp_coarse_v;
    p_final = req->coarse_result.p_qtp_coarse_v;

    set_state(DPMZM_AUTO_FINE_SCAN_P_WIDE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_QTP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_QTP,
                                DPMZM_SCAN_TARGET_P,
                                p_final,
                                req->p_fine_range_v,
                                DPMZM_AUTO_TURN_FINAL_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_P_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.p_qtp_wide_v,
                                &edge)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    p_final = result.p_qtp_wide_v;
    result.p_qtp_valid = true;

    set_state(DPMZM_AUTO_FINE_SCAN_I_WIDE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_MITP,
                                DPMZM_SCAN_TARGET_I,
                                i_final,
                                req->iq_fine_range_v,
                                DPMZM_AUTO_TURN_FINAL_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_IQ_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.i_mitp_wide_v,
                                &edge)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    i_final = result.i_mitp_wide_v;
    result.i_mitp_valid = true;

    set_state(DPMZM_AUTO_FINE_SCAN_Q_WIDE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_MITP,
                                DPMZM_SCAN_TARGET_Q,
                                q_final,
                                req->iq_fine_range_v,
                                DPMZM_AUTO_TURN_FINAL_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_IQ_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.q_mitp_wide_v,
                                &edge)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    q_final = result.q_mitp_wide_v;
    result.q_mitp_valid = true;

    set_state(DPMZM_AUTO_FINE_SCAN_P_FINE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_QTP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_QTP,
                                DPMZM_SCAN_TARGET_P,
                                p_final,
                                req->p_fine_range_v,
                                DPMZM_AUTO_TURN_FINAL_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_P_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.p_qtp_fine_v,
                                &result.p_qtp_expanded)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    p_final = result.p_qtp_fine_v;

    set_state(DPMZM_AUTO_FINE_SCAN_I_FINE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_MITP,
                                DPMZM_SCAN_TARGET_I,
                                i_final,
                                DPMZM_AUTO_PRELOCK_IQ_WINDOW_V,
                                DPMZM_AUTO_PRELOCK_IQ_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_IQ_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.i_mitp_fine_v,
                                &result.i_mitp_expanded)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    i_final = result.i_mitp_fine_v;

    set_state(DPMZM_AUTO_FINE_SCAN_Q_FINE);
    if (!apply_fixed_biases(&scan_req, i_final, q_final, p_final)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    set_auto_scan_blocks(&scan_req,
                         DPMZM_SCAN_STAGE_MITP,
                         req->iq_blocks,
                         req->p_blocks);
    if (!run_turning_point_scan(&scan_req,
                                DPMZM_SCAN_STAGE_MITP,
                                DPMZM_SCAN_TARGET_Q,
                                q_final,
                                DPMZM_AUTO_PRELOCK_IQ_WINDOW_V,
                                DPMZM_AUTO_PRELOCK_IQ_WINDOW_V,
                                req->fine_step_v,
                                DPMZM_AUTO_TURN_STEP_V,
                                DPMZM_AUTO_IQ_TURN_MAX_SHIFTS,
                                req->sweep_min_v,
                                req->sweep_max_v,
                                &result.q_mitp_fine_v,
                                &result.q_mitp_expanded)) {
        result.error = DPMZM_AUTO_ERR_SCAN_EXECUTION;
        goto fail;
    }
    q_final = result.q_mitp_fine_v;

    set_state(DPMZM_AUTO_DONE);
    result.error = DPMZM_AUTO_OK;
    s_auto_ctx.error = DPMZM_AUTO_OK;
    s_auto_ctx.last_fine_result = result;
    s_auto_ctx.has_fine_result = true;
    *out = result;
    return true;

fail:
    set_state(DPMZM_AUTO_FAILED);
    s_auto_ctx.error = result.error;
    s_auto_ctx.last_fine_result = result;
    s_auto_ctx.has_fine_result = true;
    *out = result;
    return false;
}

void dpmzm_auto_print_result(const dpmzm_auto_coarse_result_t *result)
{
    if (result == NULL) {
        printf("[dpmzm][auto] no result\r\n");
        return;
    }

    printf("[dpmzm][auto] result\r\n");
    printf("  error:         %s\r\n", auto_error_name(result->error));
    printf("  I MATP ref:    %+.3fV\r\n", (double)result->i_matp_ref_v);
    printf("  Q MATP ref:    %+.3fV\r\n", (double)result->q_matp_ref_v);
    printf("  I P-init:      %+.3fV\r\n", (double)result->i_pqtp_init_v);
    printf("  Q P-init:      %+.3fV\r\n", (double)result->q_pqtp_init_v);
    printf("  P QTP coarse:  %+.3fV (%s)\r\n",
           (double)result->p_qtp_coarse_v,
           result->p_qtp_valid ? "valid" : "invalid");
    printf("  I MITP coarse: %+.3fV (%s)\r\n",
           (double)result->i_mitp_coarse_v,
           result->i_mitp_valid ? "valid" : "invalid");
    printf("  Q MITP coarse: %+.3fV (%s)\r\n",
           (double)result->q_mitp_coarse_v,
           result->q_mitp_valid ? "valid" : "invalid");
}

void dpmzm_auto_print_fine_result(const dpmzm_auto_fine_result_t *result)
{
    if (result == NULL) {
        printf("[dpmzm][auto] no fine result\r\n");
        return;
    }

    printf("[dpmzm][auto] fine result\r\n");
    printf("  error:        %s\r\n", auto_error_name(result->error));
    printf("  P QTP wide:  %+.3fV\r\n", (double)result->p_qtp_wide_v);
    printf("  P QTP seed:  %+.3fV (%s, edge-rescan=%s)\r\n",
           (double)result->p_qtp_fine_v,
           result->p_qtp_valid ? "valid" : "invalid",
           result->p_qtp_expanded ? "yes" : "no");
    printf("  I MITP wide:  %+.3fV\r\n", (double)result->i_mitp_wide_v);
    printf("  I MITP seed:  %+.3fV (%s, edge-rescan=%s)\r\n",
           (double)result->i_mitp_fine_v,
           result->i_mitp_valid ? "valid" : "invalid",
           result->i_mitp_expanded ? "yes" : "no");
    printf("  Q MITP wide:  %+.3fV\r\n", (double)result->q_mitp_wide_v);
    printf("  Q MITP seed:  %+.3fV (%s, edge-rescan=%s)\r\n",
           (double)result->q_mitp_fine_v,
           result->q_mitp_valid ? "valid" : "invalid",
           result->q_mitp_expanded ? "yes" : "no");
}

void dpmzm_auto_print_status(void)
{
    printf("[dpmzm][auto] status\r\n");
    printf("  state:         %s\r\n", auto_state_name(s_auto_ctx.state));
    printf("  error:         %s\r\n", auto_error_name(s_auto_ctx.error));
    printf("  retry_count:   %lu\r\n", (unsigned long)s_auto_ctx.retry_count);
    printf("  has_result:    %s\r\n", s_auto_ctx.has_result ? "yes" : "no");
    if (s_auto_ctx.has_result) {
        dpmzm_auto_print_result(&s_auto_ctx.last_result);
    }
    printf("  has_fine:      %s\r\n", s_auto_ctx.has_fine_result ? "yes" : "no");
    if (s_auto_ctx.has_fine_result) {
        dpmzm_auto_print_fine_result(&s_auto_ctx.last_fine_result);
    }
}
