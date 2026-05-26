# Spec 08 - MZM + DPMZM 单板集成实时状态

本文档记录 `integration/mzm-dpmzm-unified` 分支的实时集成进展。当前目标不是让 MZM 与 DPMZM 同时闭环运行，而是在同一块 STM32H523 偏压控制板和同一份固件中集成两套功能，并通过运行模式切换选择当前控制对象。

## 0. V1 集成结论

截至 2026-05-26，当前分支可以标记为：

```text
MZM + DPMZM 单板集成 V1 可用版本
```

该结论的含义是：

- 同一份固件中已经集成 MZM 与 DPMZM 两套命令命名空间。
- 当前设计目标是运行时选择 `mode mzm` 或 `mode dpmzm`，不要求 MZM 与 DPMZM 同时闭环。
- DPMZM 的导频输出、32 kSPS 测量链路、自动扫描、自动找点和闭环入口已经恢复到可用状态。
- DPMZM 集成过程中发现的 TIM6 导频频率问题和 DSP 采样率错配问题已经修复。
- 当前现场无法完整复测 MZM 闭环，因此 MZM 部分在 PR 前仍建议保留为“待主线硬件回归确认”，不要在 PR 描述中写成已经完成全量板级验证。

V1 的边界也需要明确：

- V1 不是双算法同时运行版本。
- V1 不切换 DPMZM 到 64 kSPS，继续使用已验证的 32 kSPS 路径。
- V1 不重构 MZM/DPMZM 控制算法，只完成同板、同固件、可切换集成。

## 1. 当前基线

| 项目 | 当前状态 |
| --- | --- |
| 集成分支 | `integration/mzm-dpmzm-unified` |
| 集成底座 | `upstream/main` 的新 `src/` / `scripts/` 架构 |
| DPMZM 功能来源 | `dpmzm-open-loop` |
| DPMZM 稳定参考点 | `dpmzm-known-good-2026-05-21` |
| 当前验证状态 | 2026-05-25 已完成 DPMZM pilot、DSP 采样率、扫描、自动找点和模式切换回归 |

## 2. 已完成的集成工作

| 序号 | 集成项 | 状态 | 说明 |
| --- | --- | --- | --- |
| 1 | DPMZM 代码迁移到新架构 | 已完成 | DPMZM app/control/DSP 代码已迁移到 `src/app`、`src/control` 等目录。 |
| 2 | 串口命令命名空间 | 已完成 | 保留 `mzm ...` 与 `dpmzm ...` 命令，避免命令冲突。 |
| 3 | 运行模式切换 | 已完成 | 支持 `mode mzm` / `mode dpmzm`，第一阶段只要求可切换运行，不要求双功能同时闭环。 |
| 4 | DPMZM 导频输出链路 | 已完成 | `TIM6 -> DAC/SPI/DMA -> I/Q 导频输出` 已在板上验证。 |
| 5 | TIM6 频率恢复 | 已完成 | 集成后 TIM6 曾被主线低速 housekeeping 配置影响，已在 DPMZM pilot 启动时恢复到约 16 kHz。 |
| 6 | DPMZM DSP 采样率隔离 | 已完成 | 新增 DPMZM 专用 DSP 常量，保持已验证的 32 kSPS / 640 samples 测量路径。 |
| 7 | DPMZM 自动找点回归 | 已完成 | 用户已验证自动扫描和曲线幅度恢复正常。 |
| 8 | MZM 回归 | 已完成 | 用户已验证 MZM 基本功能正常。 |
| 9 | 模式切换回归 | 已完成 | 用户已验证 MZM/DPMZM 模式切换正常。 |

## 3. 关键问题与修复记录

### 3.1 TIM6 导频频率问题

集成初期 `dpmzm status` 显示导频输出为 `continuous`，但示波器无法看到正确导频。原因是主线中 TIM6 可能被低速周期任务配置占用，导致 DPMZM pilot 调度频率不满足导频 LUT 输出需求。

修复方式：

- 在 DPMZM pilot 启动路径中检查 TIM6 实际更新率。
- 若 TIM6 不在 DPMZM 需要的约 16 kHz 更新率，则重新配置 PSC/ARR。
- `dpmzm status` 中增加 `pilot tim6 fs` 诊断信息，便于现场确认。

期望状态：

```text
dpmzm status
  pilot output: continuous
  pilot timer:  running
  pilot tim6 fs: 16000.00 Hz (ok)
```

### 3.2 DPMZM 频点整体偏低约 30 dB 问题

集成后第一次完整 DPMZM 自动扫描时，I/Q 一阶指标和 P 路交调指标整体比旧稳定版本低约 28-32 dB。该现象不是普通扫描波动，而是测量链路级别错误。

定位结论：

- 旧稳定 DPMZM 版本使用 32 kSPS 测量路径。
- 集成分支一度继承主线全局 `DSP_SAMPLE_RATE_HZ = 64000`。
- DPMZM Goertzel 仍按 `1000/1200/2200 Hz` 指标计算，但 ADC/DSP 常量不一致会导致频点落错。
- 结果表现为导频实际存在，但固件在错误的频点取幅值，因此曲线整体偏低。

修复方式：

- 新增 `src/control/ctrl_dpmzm_dsp.h`。
- DPMZM 扫描、raw capture、Goertzel block、状态打印全部使用 DPMZM 专用常量。
- 当前先保持已验证的 `32 kSPS / 640 samples` 路径，不被 MZM 全局 DSP 常量影响。

当前期望状态：

```text
dpmzm status
  adc dsp fs: 32000 Hz
```

## 4. 当前 DPMZM 专用 DSP 参数

| 参数 | 当前值 | 说明 |
| --- | --- | --- |
| `DPMZM_DSP_SAMPLE_RATE_HZ` | `32000` | DPMZM 已验证采样率。 |
| `DPMZM_DSP_PILOT_PERIOD_SAMPLES` | `32` | 1 kHz 导频每周期采样点数。 |
| `DPMZM_DSP_GOERTZEL_BLOCK_CYCLES` | `20` | 每个 Goertzel block 包含 20 个 1 kHz 周期。 |
| `DPMZM_DSP_GOERTZEL_BLOCK_SIZE` | `640` | 每个 DPMZM 频点测量块采样点数。 |
| `DPMZM_DSP_CONTROL_DECIMATION` | `10` | DPMZM 控制测量降采样参数。 |

说明：后续如果要切换到 64 kSPS，不能只改宏定义。必须同时验证 ADC 输出速率、DRDY/SPI 读数时序、Goertzel block size、脚本 FFT 采样率和扫描结果，确认 1000/1200/2200 Hz 真实频点一致后再迁移。

## 5. 板级验证状态

| 验证项 | 结果 | 备注 |
| --- | --- | --- |
| DPMZM pilot 输出 | 通过 | 示波器可观察到导频。 |
| DPMZM raw/FFT 频点 | 通过 | 频点恢复到预期位置。 |
| DPMZM 自动扫描 | 通过 | 与旧稳定版本同量级，不再整体低 30 dB。 |
| DPMZM 自动找点 | 通过 | 用户已验证。 |
| DPMZM 闭环入口 | 通过 | 当前未发现 `still running` 类卡死。 |
| MZM 基本功能 | 通过 | 用户已验证。 |
| MZM/DPMZM 模式切换 | 通过 | 用户已验证。 |

## 6. 当前仍需关注的风险

| 风险 | 说明 | 建议 |
| --- | --- | --- |
| 64 kSPS 迁移风险 | 当前 DPMZM 稳定路径是 32 kSPS，贸然切到 64 kSPS 会再次引入频点错位。 | 保持 32 kSPS，后续单独开实验分支验证 64 kSPS。 |
| 共享底层资源 | MZM 与 DPMZM 共用 DAC、ADC、SPI、DMA、TIM 等资源。 | 第一阶段保持运行时单模式，切换时明确释放/重配资源。 |
| 注释历史遗留 | 部分 ADC 注释曾写 64 kSPS，容易误导后续维护。 | 后续单独清理注释，不影响当前功能。 |
| 长时间稳定性 | 集成后还需要更长时间的 DPMZM 锁定测试。 | 完成基本集成后做 10-20 min 以上稳定性记录。 |

## 7. 下一步计划

1. 提交当前集成修复，形成可回退点。
2. 打 tag：`integration-step-04-dpmzm-pilot-dsp-ok`。
3. 保持 DPMZM 32 kSPS 稳定路径，继续完成 MZM/DPMZM 单模式回归。
4. 后续如需 64 kSPS，单独建立验证任务，不和集成主线混在一起。

## 8. 推荐现场检查命令

```text
mode dpmzm
dpmzm status
dpmzm set pilot-open on
dpmzm capture raw 3200 20
dpmzm auto lock
dpmzm auto status
dpmzm lock status

mode mzm
mzm status

mode dpmzm
dpmzm status
```

验收重点：

- `pilot tim6 fs` 约为 `16000 Hz (ok)`。
- `adc dsp fs` 显示 `32000 Hz`。
- I/Q 一阶扫描不再整体低 30 dB。
- P-QTP 交调曲线存在可识别谷底。
- 模式切换后无 SPI/DMA/TIM 残留卡死。
