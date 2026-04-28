# Scan Artifacts

Bias scan captures and derived figures are grouped here to keep `docs/` tidy.

## Layout

- `raw/`: exported CSV time-series data
- `plots/`: figures generated from the retained captures

## Retention Policy

本目录只保留当前控制算法直接相关、可复现结论的最新验证产物。
中间调参 smoke run、失败试验图、过渡迭代数据不保留在仓库中。

## 当前保留集（DPMZM 自动粗扫 + 自动细扫，2026-04-28）

| 文件 | 说明 |
|---|---|
| `2026-04-28-dpmzm-auto-coarse-fine-summary.md` | DPMZM 第二版自动细扫第一轮上板验证总结 |
| `raw/2026-04-28-auto-coarse-fine/` | 本轮串口日志、汇总 CSV、分段 CSV |
| `assets/2026-04-28-auto-coarse-fine/` | 本轮 11 段扫描曲线图 |

阶段结论：`dpmzm auto coarse` + `dpmzm auto fine` 完整跑通，最终自动应用 `I=+5.600 V, Q=-6.300 V, P=+1.520 V`，并且该点已经通过光功率计做了定性确认。

## 当前保留集（spec-04 验收，2026-04-13）

### 标定扫描（双扫描校准，spec-04 实现验证）

| 文件 | 说明 |
|------|------|
| `raw/calibration_scan_spec04_pass12_*_pass1.csv` | Pass 1 快扫数据（全范围，找 Vπ/canonical 周期） |
| `raw/calibration_scan_spec04_pass12_*_pass2.csv` | Pass 2 慢扫数据（单周期，建 affine 模型）|
| `plots/calibration_scan_spec04_pass12_*_pass2.png` | Pass 2 H1/H2 曲线图 |
| `plots/calibration_scan_spec04_pass12_*_pass2_analysis.png` | Pass 2 残差与仿射拟合分析图 |

### 六工作点套测（spec-04 最终验收）

| 文件（`raw/*.csv` + `plots/*.png`）| 工作点 | 结果 |
|---|---|---|
| `lock_response_quad_suite_2026-04-13_091730` | QUAD 90° | 90.28° ±0.10°，首锁 0.0 s |
| `lock_response_max_suite_2026-04-13_091906` | MAX 180° | 178.07° ±1.16°，首锁 0.5 s |
| `lock_response_min_suite_2026-04-13_091953` | MIN 0° | 0.15° ±1.15°，首锁 0.5 s |
| `lock_response_custom_45deg_suite_2026-04-13_092040` | CUSTOM 45° | 42.46° ±1.02°，首锁 0.0 s |
| `lock_response_custom_135deg_suite_2026-04-13_092128` | CUSTOM 135° | 136.36° ±0.60°，首锁 0.5 s |
| `lock_response_custom_17deg_suite_2026-04-13_092215` | CUSTOM 17° | 16.21° ±1.01°，首锁 0.5 s |

标定参数：Vπ = 5.450 V，DC null = −0.001 V，DC peak = 1.076 V

### 历史参考图（spec-02 DSP 表征，只读）

| 文件 | 说明 |
|------|------|
| `plots/vpi_scan_100mvpp_full_3blk_2026-04-02.png` | Vπ 全范围扫描（100 mVpp 导频） |
| `plots/scan_plot_50mvpp.png` | 50 mVpp 导频下 H1/H2 响应曲线 |
| `plots/scan_plot_100mvpp.png` | 100 mVpp 导频下 H1/H2 响应曲线 |
| `plots/filtered_h2_scan_50mvpp_3blk_2026-04-03.png` | H2 滤波前后对比（50 mVpp） |
| `plots/filtered_h2_scan_100mvpp_3blk_2026-04-03.png` | H2 滤波前后对比（100 mVpp） |

## Naming Convention

```
lock_response_<target>_<tag>_<timestamp>.{csv,png}
calibration_scan_<tag>_<timestamp>_<pass>.{csv,png}
```

新产生的验证批次在结论稳定后替换当前保留集，不持续累积历史产物。
