# Bias Controller — Development Plan Index

## Project Goal
光电调制器偏压控制器固件。通过导频抖动 + Goertzel 数字锁相实现闭环偏压控制。
先完成 MZM (quad/max/min) demo，后续扩展到 DDMZM、PM、DPMZM、DPQPSK。

## Spec Files

| Spec | Status | Description |
|------|--------|-------------|
| [spec-00-hardware](spec-00-hardware.md) | **Reference** | Hardware pin mapping, signal chain, CubeMX config, NVIC, DMA |
| [spec-01-bringup](spec-01-bringup.md) | **COMPLETE** ✅ | Board bring-up: DAC8568, ADS131M02, USART1 drivers |
| [spec-02-dsp-pipeline](spec-02-dsp-pipeline.md) | **COMPLETE** ✅ | Pilot tone + Goertzel extraction + Vπ characterization |
| [spec-03-mzm-quad](spec-03-mzm-quad.md) | **Complete (milestone)** | MZM full-range operating point control baseline, kept as previous-stage reference |
| [spec-04-mzm-no-dc-5hz](spec-04-mzm-no-dc-5hz.md) | **COMPLETE** ✅ | 5 Hz dual-scan, all-target no-DC control — validated 2026-04-13 |
| [spec-05-robustness](spec-05-robustness.md) | Pending | Robustness, tuning interface, parameter persistence |
| [spec-06-multi-modulator](spec-06-multi-modulator.md) | Future | DDMZM, DPMZM, DPQPSK, PM support |
| [spec-06a-dpmzm-migration-checklist](spec-06a-dpmzm-migration-checklist.md) | Draft | DPMZM 改造清单：基于当前 MZM 工程真实代码结构的落地路线 |
| [spec-06b-dpmzm-parallel-files](spec-06b-dpmzm-parallel-files.md) | Draft | DPMZM 并行新增文件清单：在不破坏原 MZM 主线前提下扩展 |
| [spec-06c-dpmzm-first-batch-files](spec-06c-dpmzm-first-batch-files.md) | Draft | DPMZM 第一批创建文件清单：先做最小开环实验集合 |

## Key Technical Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Pilot frequency f0 | 1 kHz | Well below RF band |
| Pilot amplitude | ~50 mV (DAC ~164 LSB) | After 4x subtractor gain |
| ADC sample rate | 64 kSPS | ADS131M02 OSR=128, HR mode, 8.192 MHz CLKIN (verified empirically) |
| Pilot base period | 64 samples | 1 cycle at 64 kSPS × 1 kHz |
| Goertzel block N | 1280 | 20 coherent pilot cycles/block (20 ms window) |
| Control loop rate | ~5 Hz | 10 Goertzel blocks/update (200 ms latency) |
| HSE crystal | 8.192 MHz | Divides evenly for ADC CLKIN; MCO1 on PA8 |
| FPU | FPv5-SP (hard float) | No software emulation needed |

## Architecture

```
APP       → app/        State machine, config
CONTROL   → control/    Bias loop, modulator strategies, PID
DSP       → dsp/        Goertzel, pilot sine gen (pure math, host-testable)
DRIVER    → drivers/    ADS131M02, DAC8568, board GPIO
HAL       → cubemx/     CubeMX generated (do NOT edit)
```

## Data Flow

```
DAC8568(bias + pilot) → subtractor → modulator bias electrode
                                          |
PD → TIA(OPA140) → ADS131M02 CH0 → Goertzel(f0, 2f0)
                        ADS131M02 CH1 → DC mean
                                          |
                              error → PID → update bias
```

## Measurement Artifacts

- Scan raw data and derived plots live under `docs/scans/`
- Raw CSV captures: `docs/scans/raw/`
- Generated figures: `docs/scans/plots/`
- Repository keeps only the current retained validation set; see `docs/scans/README.md`

## DPMZM Working Notes

- [spec-06d-dpmzm-status-overview](spec-06d-dpmzm-status-overview.md): DPMZM 总览页，集中说明目标、已实现、未实现、代码入口与下一步
- [spec-06e-dpmzm-uart-checklist](spec-06e-dpmzm-uart-checklist.md): 明天上板实验时可直接照着执行的 DPMZM 串口命令清单
- [spec-06f-dpmzm-closed-loop-strategy](spec-06f-dpmzm-closed-loop-strategy.md): 基于现有开环数据整理的 DPMZM 可行控制方案，区分粗捕获、局部细化与真正闭环
- [spec-06g-dpmzm-v1-implementation-checklist](spec-06g-dpmzm-v1-implementation-checklist.md): 第一版固件实现清单，只聚焦“板上自动粗捕获”
- [spec-06h-dpmzm-v1-code-change-checklist](spec-06h-dpmzm-v1-code-change-checklist.md): 第一版代码改动清单，明确当前代码库里要改哪些文件、先加哪些结构体和接口
