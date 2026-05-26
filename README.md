# biascontrol_h523

STM32H523 光电调制器自动偏压控制固件。

本项目面向 MZM / DPMZM 电光调制器偏压控制实验平台，使用 DAC 输出偏压和低频导频，使用 ADC 采集 PD/TIA 输出，并通过 Goertzel 频点提取、自动找点和闭环控制维持调制器工作点。

当前集成分支已经完成第一版 **MZM + DPMZM 单板可切换运行**：

```text
same board + same firmware + selectable MZM/DPMZM runtime mode
```

第一版目标不是让 MZM 和 DPMZM 同时闭环，而是在同一固件中保留两套功能，并通过运行模式选择当前控制对象。

## 当前状态

| 项目 | 状态 |
| --- | --- |
| 当前集成分支 | `integration/mzm-dpmzm-unified` |
| 集成底座 | `upstream/main` 新 `src/` / `scripts/` 架构 |
| DPMZM 稳定来源 | `dpmzm-open-loop` |
| 当前集成 tag | `integration-v1-mzm-dpmzm-switchable` |
| DPMZM pilot | 已验证，TIM6 约 16 kHz 更新 |
| DPMZM 测量链路 | 已验证，32 kSPS / 640 samples |
| DPMZM 自动找点和闭环入口 | 已验证 |
| MZM 功能 | 已集成，完整板级闭环仍建议在 MZM 光路上复测 |

重要边界：

- 当前版本支持 `mode mzm` 和 `mode dpmzm` 切换运行。
- 当前版本不要求 MZM 和 DPMZM 同时闭环。
- DPMZM 继续使用已验证的 32 kSPS 测量路径，不直接切换到 64 kSPS。
- 若未来要迁移 DPMZM 到 64 kSPS，需要同时验证 ADC 输出速率、SPI/DRDY 时序、Goertzel block size、脚本 FFT 采样率和扫描曲线。

## 系统结构

```text
DAC8568 bias/pilot output
        |
        v
MZM / DPMZM bias electrodes
        |
        v
Optical output -> PD/TIA -> ADS131M02 ADC
        |
        v
Goertzel / DC metrics
        |
        v
Auto scan / auto find / closed-loop control
        |
        v
DAC8568 bias update
```

### DPMZM 控制目标

DPMZM 由 I/Q 两个子 MZM 和 P 路父级相位控制结构组成。当前控制目标为：

```text
I-MITP + Q-MITP + P-QTP
```

对应指标：

| 通道 | 工作点 | 主指标 |
| --- | --- | --- |
| I | I-MITP，I 子调制器最小传输点 | 1000 Hz 一阶导频项最小 |
| Q | Q-MITP，Q 子调制器最小传输点 | 1200 Hz 一阶导频项最小 |
| P | P-QTP，I/Q 合成相位正交点 | 2200 Hz 和频交调项最小 |

说明：

- 200 Hz 差频项在实验中容易受低频扰动影响，当前主要使用 2200 Hz 作为 P 路 QTP 指标。
- DPMZM 导频用于偏压探测，不是通信 RF 信号。
- P-QTP 在无 RF 和有 RF 条件下表现可能不同，工程测试应以真实工作光路为准。

## 硬件平台

| 模块 | 型号 / 说明 |
| --- | --- |
| MCU | STM32H523CET6 |
| DAC | DAC8568，输出偏压和导频 |
| ADC | ADS131M02，采集 AC/DC 光电信号 |
| PD/TIA | 光电探测与跨阻放大链路 |
| 串口 | USART1，115200 8N1 |
| DPMZM pilot timer | TIM6，DPMZM 模式下恢复到约 16 kHz |
| 主要 DMA/SPI | SPI1/DAC，SPI2/ADC，GPDMA |

## 软件结构

```text
src/
  app/        应用入口、模式切换、串口命令、DPMZM app 层
  control/    MZM/DPMZM 控制算法、扫描、自动找点、闭环
  dsp/        Goertzel、导频、DSP 类型
  drivers/    DAC8568、ADS131M02、board callbacks
cubemx/       CubeMX/HAL 生成代码
scripts/      烧录、扫描采集、画图、实验辅助脚本
docs/plan/    设计规格、集成计划、状态记录
test/         主机端单元测试
```

## 构建

推荐使用 CMake + Ninja + arm-none-eabi-gcc。

```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j 8
```

常见输出：

```text
build/biascontrol.elf
build/biascontrol.hex
build/biascontrol.bin
```

## 烧录

仓库提供了一键构建和烧录脚本：

```powershell
scripts\flash_current_firmware.bat
```

默认参数：

```text
delay_seconds = 2
connect_mode  = under-reset
frequency_hz  = 1000000
```

如果需要手动配合 RESET：

```powershell
scripts\flash_current_firmware.bat 3 under-reset 100000
```

也可以直接使用 pyOCD：

```powershell
pyocd flash -t stm32h523cetx --connect under-reset --frequency 1000000 build\biascontrol.hex
```

## 串口快速检查

串口参数：

```text
115200 8N1
```

基础命令：

```text
status
mode mzm
mzm status
mode dpmzm
dpmzm status
```

DPMZM 关键状态期望：

```text
dpmzm status
  initialized: yes
  adc dsp fs: 32000 Hz
  pilot output: continuous 或 scan-only
  pilot tim6 fs: 16000.00 Hz (ok)
```

## DPMZM 常用命令

```text
mode dpmzm
dpmzm status
dpmzm set pilot-open on
dpmzm set pilot-open off
dpmzm set pilot-src onboard
dpmzm set dump metrics
dpmzm set bias i 0
dpmzm set bias q 0
dpmzm set bias p 0
```

单次扫描：

```text
dpmzm scan matp i -9.0 9.0 0.5 4
dpmzm scan matp q -9.0 9.0 0.5 4
dpmzm scan qtp p -9.0 9.0 0.5 10
dpmzm scan mitp i -9.0 9.0 0.5 4
dpmzm scan mitp q -9.0 9.0 0.5 4
```

自动找点和闭环：

```text
dpmzm auto lock
dpmzm auto status
dpmzm lock status
dpmzm lock stop
```

原始数据采集：

```text
dpmzm capture raw 3200 20
```

## 脚本工具

### 单次 DPMZM 扫描、保存数据并画图

```powershell
scripts\run_dpmzm_scan_and_plot.bat
```

脚本会提示输入串口和扫描命令，例如：

```text
dpmzm scan matp i -9.0 9.0 0.5 4
```

输出包括：

- 串口原始日志
- 进度日志
- metrics CSV
- 扫描曲线图

默认保存位置：

```text
C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data
C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\manual_scan
```

## DPMZM 集成中的关键修复

### 1. TIM6 导频频率修复

集成初期出现过 `dpmzm status` 显示导频打开，但示波器看不到正确导频的问题。原因是 TIM6 可能被主线低速周期任务配置影响，导致 DPMZM pilot LUT 更新率不正确。

修复后：

- DPMZM pilot 启动时检查 TIM6。
- 若 TIM6 不满足 DPMZM pilot 更新率要求，则重新配置为约 16 kHz。
- `dpmzm status` 打印 `pilot tim6 fs` 作为现场诊断信息。

### 2. DPMZM 32 kSPS 测量链路隔离

集成初期出现过 I/Q/P 指标整体比旧稳定版本低约 30 dB 的问题。根因是 DPMZM 误用了主线全局 64 kSPS DSP 常量，导致 Goertzel 频点错位。

修复后新增：

```text
src/control/ctrl_dpmzm_dsp.h
```

DPMZM 专用参数：

```text
DPMZM_DSP_SAMPLE_RATE_HZ        = 32000
DPMZM_DSP_PILOT_PERIOD_SAMPLES  = 32
DPMZM_DSP_GOERTZEL_BLOCK_SIZE   = 640
```

这样 DPMZM 保持旧分支已验证的 32 kSPS 测量路径，同时不强行修改 MZM 的全局 DSP 设计。

## 测试建议

PR 或合入主线前建议至少检查：

```text
mode dpmzm
dpmzm status
dpmzm set pilot-open on
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
- MZM/DPMZM 模式切换后无 SPI/DMA/TIM 残留卡死。

## 重要文档

| 文档 | 内容 |
| --- | --- |
| [spec-08-mzm-dpmzm-unified-integration-plan.md](docs/plan/spec-08-mzm-dpmzm-unified-integration-plan.md) | MZM + DPMZM 单板集成计划 |
| [spec-08-integration-status.md](docs/plan/spec-08-integration-status.md) | 当前集成实时状态 |
| [spec-08-pr-merge-checklist.md](docs/plan/spec-08-pr-merge-checklist.md) | PR / merge 检查清单 |
| [spec-08-pr-body.md](docs/plan/spec-08-pr-body.md) | 可复制的 PR 正文 |

## 回退点

| tag | 说明 |
| --- | --- |
| `dpmzm-known-good-2026-05-21` | DPMZM 旧稳定功能来源 |
| `integration-step-04-dpmzm-pilot-dsp-ok` | DPMZM pilot 和 DSP 采样率修复后的集成回退点 |
| `integration-v1-mzm-dpmzm-switchable` | MZM + DPMZM 单板可切换 V1 版本 |

## 后续工作

- 在具备 MZM 光路条件时补做 MZM 完整闭环回归。
- 继续观察 DPMZM 长时间闭环稳定性。
- 若需要 64 kSPS，单独建立实验分支验证，不直接改动当前 V1 稳定路径。
- 合入主线后清理历史注释和旧文档中可能误导的 64 kSPS 描述。
