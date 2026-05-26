# Spec 08 - MZM + DPMZM 单板集成 PR/Merge 检查清单

本文档用于准备 `integration/mzm-dpmzm-unified` 分支合入主线前的 PR 描述、测试记录和回退策略。

## 1. PR 标题建议

```text
Integrate switchable MZM and DPMZM runtime on STM32H523 bias board
```

中文说明：

```text
集成 MZM/DPMZM 单板可切换运行固件
```

## 2. PR 目标

本次 PR 的目标是把 MZM 与 DPMZM 两套偏压控制功能集成到同一份固件中，并通过运行模式选择当前启用的控制对象。

本次 PR 不追求：

- MZM 与 DPMZM 同时闭环运行。
- DPMZM 采样率切换到 64 kSPS。
- 重写 MZM 或 DPMZM 的控制算法。
- 合并两套算法状态机。

## 3. 主要变更点

| 模块 | 变更内容 |
| --- | --- |
| Shell 命令 | 保留 `mzm ...` 与 `dpmzm ...` 命名空间，增加模式切换入口。 |
| DPMZM app/control | 将 DPMZM 自动扫描、自动找点、闭环入口迁移到主线 `src/` 架构。 |
| SPI/DMA 回调 | 在统一固件中路由 DAC、DPMZM pilot 等共享 SPI/DMA 回调。 |
| TIM6 导频 | DPMZM pilot 启动时恢复 TIM6 到约 16 kHz，避免被主线低速 timer 配置影响。 |
| DPMZM DSP | 新增 DPMZM 专用 32 kSPS 测量常量，避免被 MZM 全局 DSP 采样率误伤。 |
| 工具脚本 | 增加一键烧录脚本、DPMZM 单次扫描并自动保存/画图脚本。 |
| 文档 | 增加集成计划、集成实时状态和 PR 检查清单。 |

## 4. 关键修复说明

### 4.1 DPMZM pilot 无输出

现象：

- `dpmzm status` 显示 `pilot output: continuous`。
- 示波器上看不到正确 I/Q 导频。

原因：

- 集成后 TIM6 可能继承主线低速 housekeeping 配置。
- DPMZM pilot LUT 需要约 16 kHz 更新率，低速 TIM6 会导致导频无法按预期生成。

修复：

- DPMZM pilot 启动时检查并恢复 TIM6 PSC/ARR。
- `dpmzm status` 打印 `pilot tim6 fs` 用于现场确认。

### 4.2 DPMZM 扫描幅度整体低约 30 dB

现象：

- 集成分支自动扫描曲线整体比旧稳定 DPMZM 分支低约 28-32 dB。
- I/Q 一阶和 P 路交调指标同时偏低。

原因：

- 旧稳定 DPMZM 测量链路为 32 kSPS。
- 集成分支误用主线全局 64 kSPS DSP 常量。
- Goertzel 按错误采样率寻找 1000/1200/2200 Hz，导致频点错位。

修复：

- 增加 `src/control/ctrl_dpmzm_dsp.h`。
- DPMZM 扫描、raw capture、Goertzel block 和状态打印统一使用 DPMZM 专用 32 kSPS 参数。

## 5. 已完成验证

| 验证项 | 状态 | 说明 |
| --- | --- | --- |
| 编译 | 通过 | `cmake --build build -j 8` 通过。 |
| DPMZM pilot | 通过 | 示波器确认导频输出恢复。 |
| DPMZM status | 通过 | `pilot tim6 fs` 显示约 16 kHz，`adc dsp fs` 显示 32 kHz。 |
| DPMZM 自动扫描 | 通过 | 频点幅度恢复到旧稳定版本同量级。 |
| DPMZM 自动找点 | 通过 | 用户已完成板级验证。 |
| DPMZM 闭环入口 | 通过 | 当前未复现卡死类问题。 |
| 模式切换 | 通过 | 用户已验证基本切换正常。 |
| MZM 完整板级闭环 | 待确认 | 当前现场暂时无法测试 MZM，应在 PR 中明确说明。 |

## 6. PR 前建议补充验证

如果具备 MZM 硬件条件，合入前建议补跑：

```text
mode mzm
mzm status
```

如果具备 DPMZM 光路条件，建议补跑：

```text
mode dpmzm
dpmzm status
dpmzm set pilot-open on
dpmzm auto lock
dpmzm auto status
dpmzm lock status
```

## 7. 回退点

| tag / commit | 用途 |
| --- | --- |
| `dpmzm-known-good-2026-05-21` | DPMZM 旧稳定功能来源。 |
| `integration-step-04-dpmzm-pilot-dsp-ok` | DPMZM pilot 和 DSP 采样率修复后的集成回退点。 |
| `integration-v1-mzm-dpmzm-switchable` | MZM + DPMZM 单板可切换 V1 版本。 |

## 8. 推荐 PR 描述

```text
This PR integrates MZM and DPMZM bias-control flows into one STM32H523 firmware.

The goal of this first integration version is runtime selection, not simultaneous
dual closed-loop operation. The shell keeps separate `mzm ...` and `dpmzm ...`
namespaces and allows switching between modes.

Major fixes during integration:
- DPMZM TIM6 pilot output is restored to the validated 16 kHz update path.
- DPMZM scan/Goertzel measurement is isolated to the validated 32 kSPS path.
- DPMZM no longer inherits the MZM/global 64 kSPS DSP constants, which previously
  shifted measurement bins and reduced measured tone levels by about 30 dB.

Board verification:
- DPMZM pilot output verified on oscilloscope.
- DPMZM scan and auto-lock flow verified on board.
- Runtime mode switching smoke test passed.
- Full MZM hardware regression is still recommended before final production use.
```
