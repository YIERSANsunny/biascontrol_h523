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
#define DPMZM_AUTO_DBM_FLOOR_MW               1e-15f

static dpmzm_auto_context_t s_auto_ctx;
static dpmzm_scan_point_t s_scan_points[DPMZM_AUTO_SCAN_POINTS_MAX];

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

static void sort_matp_by_bias(dpmzm_matp_candidate_t *candidates, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < count; i++) {
        for (j = i + 1U; j < count; j++) {
            if (candidates[j].bias_v < candidates[i].bias_v) {
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

        for (i = 1U; i + 1U < point_count; i++) {
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

static bool pick_sensitive_regions(const dpmzm_scan_point_t *points,
                                   uint32_t point_count,
                                   dpmzm_matp_candidate_t *matp_candidates,
                                   uint32_t matp_count,
                                   dpmzm_scan_target_t target,
                                   float step_v,
                                   dpmzm_sensitive_region_t *main_region,
                                   dpmzm_sensitive_region_t *backup_region)
{
    dpmzm_sensitive_region_t best = {0};
    dpmzm_sensitive_region_t second = {0};
    uint32_t i;

    memset(main_region, 0, sizeof(*main_region));
    memset(backup_region, 0, sizeof(*backup_region));

    if (points == NULL || matp_candidates == NULL || matp_count == 0U) {
        return false;
    }
    if (matp_count < 2U) {
        return pick_sensitive_region_fallback(points,
                                              point_count,
                                              target,
                                              matp_candidates,
                                              matp_count,
                                              step_v,
                                              main_region,
                                              backup_region);
    }

    sort_matp_by_bias(matp_candidates, matp_count);
    for (i = 0U; i + 1U < matp_count; i++) {
        uint32_t left_idx = matp_candidates[i].point_index;
        uint32_t right_idx = matp_candidates[i + 1U].point_index;
        uint32_t j;
        uint32_t peak_idx = left_idx;
        float peak_metric = -FLT_MAX;
        dpmzm_sensitive_region_t region;

        if (right_idx <= left_idx + 1U || right_idx >= point_count) {
            continue;
        }

        memset(&region, 0, sizeof(region));
        region.left_matp_v = matp_candidates[i].bias_v;
        region.right_matp_v = matp_candidates[i + 1U].bias_v;

        for (j = left_idx; j <= right_idx; j++) {
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

        region.plateau_peak_dbm = peak_metric;
        region.plateau_left_v = points[peak_idx].sweep_v;
        region.plateau_right_v = points[peak_idx].sweep_v;

        for (j = peak_idx; j > left_idx; j--) {
            float metric = smoothed_metric_dbm(DPMZM_SCAN_STAGE_MATP,
                                               target,
                                               points,
                                               point_count,
                                               j - 1U);
            if ((peak_metric - metric) > DPMZM_AUTO_PLATEAU_DROP_DB) {
                break;
            }
            region.plateau_left_v = points[j - 1U].sweep_v;
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
            region.plateau_right_v = points[j + 1U].sweep_v;
        }

        region.plateau_center_v = 0.5f * (region.plateau_left_v + region.plateau_right_v);
        region.plateau_width_v = region.plateau_right_v - region.plateau_left_v;
        region.valid = true;

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

static bool qtp_curve_usable(const dpmzm_scan_point_t *points, uint32_t point_count)
{
    float min_metric = FLT_MAX;
    float max_metric = -FLT_MAX;
    uint32_t i;

    if (points == NULL || point_count == 0U) {
        return false;
    }

    for (i = 0U; i < point_count; i++) {
        float metric = metric_dbm_from_point(DPMZM_SCAN_STAGE_QTP,
                                             DPMZM_SCAN_TARGET_P,
                                             &points[i]);
        if (metric < min_metric) {
            min_metric = metric;
        }
        if (metric > max_metric) {
            max_metric = metric;
        }
    }

    return (max_metric - min_metric) >= DPMZM_AUTO_MIN_QTP_RANGE_DB;
}

static bool pick_qtp_best_point(const dpmzm_scan_point_t *points,
                                uint32_t point_count,
                                float *best_p_v)
{
    float best_metric = FLT_MAX;
    float best_value = 0.0f;
    uint32_t i;

    if (points == NULL || point_count == 0U || best_p_v == NULL) {
        return false;
    }

    for (i = 0U; i < point_count; i++) {
        float metric = metric_dbm_from_point(DPMZM_SCAN_STAGE_QTP,
                                             DPMZM_SCAN_TARGET_P,
                                             &points[i]);
        if (metric < best_metric) {
            best_metric = metric;
            best_value = points[i].sweep_v;
        }
    }

    *best_p_v = best_value;
    return true;
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

        for (i = 1U; i + 1U < point_count; i++) {
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

    if (candidate_count >= 2U && candidates[1U].dc_mean_v < candidates[0U].dc_mean_v) {
        selected_index = 1U;
    }
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
    dpmzm_sensitive_region_t i_main_region;
    dpmzm_sensitive_region_t i_backup_region;
    dpmzm_sensitive_region_t q_main_region;
    dpmzm_sensitive_region_t q_backup_region;
    uint32_t point_count = 0U;
    uint32_t i_matp_count = 0U;
    uint32_t q_matp_count = 0U;
    bool qtp_ok = false;
    bool used_retry = false;

    if (!request_valid(req) || out == NULL) {
        return false;
    }

    clear_context();
    s_auto_ctx.last_request = *req;
    reset_result(&result);
    memset(&scan_req, 0, sizeof(scan_req));
    memset(i_matp, 0, sizeof(i_matp));
    memset(q_matp, 0, sizeof(q_matp));
    memset(&i_main_region, 0, sizeof(i_main_region));
    memset(&i_backup_region, 0, sizeof(i_backup_region));
    memset(&q_main_region, 0, sizeof(q_main_region));
    memset(&q_backup_region, 0, sizeof(q_backup_region));
    scan_req = req->scan_template;

    set_state(DPMZM_AUTO_SCAN_I_MATP);
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
    if (i_matp_count == 0U) {
        result.error = DPMZM_AUTO_ERR_I_MATP_NOT_FOUND;
        goto fail;
    }
    result.i_matp_ref_v = i_matp[0].bias_v;

    set_state(DPMZM_AUTO_PICK_I_PLATEAU);
    if (!pick_sensitive_regions(s_scan_points,
                                point_count,
                                i_matp,
                                i_matp_count,
                                DPMZM_SCAN_TARGET_I,
                                req->sweep_step_v,
                                &i_main_region,
                                &i_backup_region)) {
        result.error = DPMZM_AUTO_ERR_I_PLATEAU_NOT_FOUND;
        goto fail;
    }
    result.i_pqtp_init_v = i_main_region.plateau_center_v;
    printf("[dpmzm][auto] I plateau center: %+.3fV\r\n",
           (double)result.i_pqtp_init_v);

    set_state(DPMZM_AUTO_SCAN_Q_MATP);
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
    if (q_matp_count == 0U) {
        result.error = DPMZM_AUTO_ERR_Q_MATP_NOT_FOUND;
        goto fail;
    }
    result.q_matp_ref_v = q_matp[0].bias_v;

    set_state(DPMZM_AUTO_PICK_Q_PLATEAU);
    if (!pick_sensitive_regions(s_scan_points,
                                point_count,
                                q_matp,
                                q_matp_count,
                                DPMZM_SCAN_TARGET_Q,
                                req->sweep_step_v,
                                &q_main_region,
                                &q_backup_region)) {
        result.error = DPMZM_AUTO_ERR_Q_PLATEAU_NOT_FOUND;
        goto fail;
    }
    result.q_pqtp_init_v = q_main_region.plateau_center_v;
    printf("[dpmzm][auto] Q plateau center: %+.3fV\r\n",
           (double)result.q_pqtp_init_v);

retry_p_qtp:
    set_state(DPMZM_AUTO_SCAN_P_QTP);
    scan_req.bias_i_v = result.i_pqtp_init_v;
    scan_req.bias_q_v = result.q_pqtp_init_v;
    scan_req.bias_p_v = req->scan_template.bias_p_v;
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
    qtp_ok = qtp_curve_usable(s_scan_points, point_count) &&
             pick_qtp_best_point(s_scan_points, point_count, &result.p_qtp_coarse_v);
    if (!qtp_ok) {
        if (!used_retry && i_backup_region.valid) {
            used_retry = true;
            s_auto_ctx.retry_count = 1U;
            result.i_pqtp_init_v = i_backup_region.plateau_center_v;
            printf("[dpmzm][auto] retry P-QTP with I backup plateau: %+.3fV\r\n",
                   (double)result.i_pqtp_init_v);
            goto retry_p_qtp;
        }
        if (!used_retry && q_backup_region.valid) {
            used_retry = true;
            s_auto_ctx.retry_count = 1U;
            result.q_pqtp_init_v = q_backup_region.plateau_center_v;
            printf("[dpmzm][auto] retry P-QTP with Q backup plateau: %+.3fV\r\n",
                   (double)result.q_pqtp_init_v);
            goto retry_p_qtp;
        }
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
}
