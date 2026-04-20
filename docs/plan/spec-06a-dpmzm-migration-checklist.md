# Spec 06A - DPMZM 改造清单

> 状态: Draft  
> 目标: 基于当前 `biascontrol_h523` 的 MZM 固件，整理出后续改造成 DPMZM 开环实验固件的实际入口、缺口和实施顺序。  
> 依赖: `spec-06-multi-modulator.md` 仅提供方向，本文件以当前代码真实结构为准。  

## 1. 文档目的

当前工程在架构上为 DPMZM 留了扩展口，但代码主体仍然是单路 MZM。

后续如果直接按 `spec-06-multi-modulator.md` 的文字去改，容易高估当前“可直接扩展”的程度。本清单的作用是明确三件事：

1. 代码里已经存在的 DPMZM 预留接口有哪些。
2. 哪些地方只是“预留”，但实际上还没有能力支撑 DPMZM。
3. 第一轮应该先把工程改到什么程度，才能支撑真实光链路的开环实验。

## 2. 当前已经存在的 DPMZM 预留接口

这些接口是真实存在于代码中的，可以作为后续改造入口。

### 2.1 调制器类型枚举已经预留

文件: `control/inc/ctrl_modulator.h`

- `modulator_type_t` 中已经包含:
  - `MOD_TYPE_MZM`
  - `MOD_TYPE_DDMZM`
  - `MOD_TYPE_PM`
  - `MOD_TYPE_DPMZM`
  - `MOD_TYPE_DPQPSK`

这说明“多调制器类型切换”在类型系统层面已经预留。

### 2.2 策略接口已经抽象完成

文件: `control/inc/ctrl_modulator.h`

`modulator_strategy_t` 已经提供了适合未来 DPMZM 扩展的通用字段：

- `compute_error()`
- `is_locked()`
- `init()`
- `bias_channels[4]`
- `num_bias_channels`
- `pilot_channel`
- `sweep_start_v / sweep_end_v / sweep_step_v`
- `target_phase_rad`

这套接口的意义是：未来新增 `ctrl_modulator_dpmzm.c` 时，可以沿用统一的 strategy 注册方式。

### 2.3 调制器策略注册入口已经存在

文件:

- `control/inc/ctrl_modulator.h`
- `control/src/ctrl_modulator_mzm.c`

当前代码已经有统一入口:

```c
const modulator_strategy_t *modulator_get_strategy(modulator_type_t type);
```

后续 DPMZM 需要做的是：

1. 新增 `ctrl_modulator_dpmzm.c/.h`
2. 在 `modulator_get_strategy()` 里注册 `MOD_TYPE_DPMZM`

### 2.4 UART 层已经给“切换调制器类型”留了命令入口

文件: `app/src/app_main.c`

已经存在命令分支：

```text
set mod <type>
```

但目前只真正支持 `mzm`。代码里已经明确写了：

```c
/* Future: add other modulator types */
```

所以这里是未来接入 `dpmzm` 的直接入口。

### 2.5 文档规划里已经写明 DPMZM 方向

文件: `docs/plan/spec-06-multi-modulator.md`

当前规划中已经明确：

- 未来新增 `control/src/ctrl_modulator_dpmzm.c`
- DPMZM 需要三路偏压
- DPMZM 需要多频导频
- DPMZM 需要顺序锁定

这说明“架构意图”是清楚的，只是代码还没落地。

## 3. 当前还没有实现的关键能力

下面这些地方，是后续 DPMZM 改造的真正难点。

### 3.1 `modulator_get_strategy()` 目前只返回 MZM

文件: `control/src/ctrl_modulator_mzm.c`

虽然接口里有 `MOD_TYPE_DPMZM`，但当前注册逻辑实际上只实现了 `MOD_TYPE_MZM`。

结论:

- DPMZM 的类型枚举已经存在
- DPMZM 的真实策略实现还不存在

### 3.2 `app_config` 仍然是单偏压、单导频模型

文件:

- `app/inc/app_config.h`
- `app/src/app_config.c`

当前配置只支持：

- 一个 `bias_dac_channel`
- 一个 `pilot_freq_hz`
- 一个 `pilot_amplitude_v`

这和 DPMZM 所需的模型不一致。DPMZM 至少需要：

- `bias_i_channel`
- `bias_q_channel`
- `bias_p_channel`
- `pilot_i_freq_hz`
- `pilot_q_freq_hz`
- `pilot_i_amplitude_v`
- `pilot_q_amplitude_v`
- `pilot_source_mode`

### 3.3 DSP 层当前仍是单导频假设

文件:

- `dsp/inc/dsp_types.h`
- `control/src/ctrl_bias.c`

当前系统常量和控制路径固定使用：

- `DSP_PILOT_FREQ_HZ`
- `DSP_PILOT_FREQ_HZ * 2`

控制器实时提取的是：

- `H1 @ f0`
- `H2 @ 2f0`

而 DPMZM 第一阶段至少需要同时看到：

- `fI`
- `fQ`
- `|fI-fQ|`
- `fI+fQ`

所以现有 DSP/控制接口不能直接拿来做 DPMZM，只能复用 Goertzel 内核本身。

### 3.4 `ctrl_bias.c` 名义上支持多 bias channel，实际上只用了第一个

文件: `control/src/ctrl_bias.c`

虽然 strategy 结构里有：

- `bias_channels[4]`
- `num_bias_channels`

但在 coarse sweep 以及主控制路径里，当前实际只取：

```c
ctrl->strategy->bias_channels[0]
```

这意味着当前控制器仍然是“单通道偏压控制器”，并没有真正调度多个 bias 通道。

### 3.5 `app_main.c` 中大量 MZM 逻辑已经写死

文件: `app/src/app_main.c`

目前应用层不只是状态机，还直接包含：

- `cal bias`
- `scan vpi`
- `scan harmonics`
- `null / peak / quad+ / quad-` 锚点分析
- affine 拟合
- harmonic-axis calibration
- 针对 MZM 的 seed / clamp / lock 起步流程

所以后续若改 DPMZM，不能只加一个新 strategy 文件。应用层扫描流程也必须改。

### 3.6 当前 spec-06 有一句话不适用于现状

文件: `docs/plan/spec-06-multi-modulator.md`

原文写的是：

> No changes needed in `ctrl_bias.c` or `app_main.c`

这句话和当前代码现状不一致。  
按现在的实现，DPMZM 至少一定会改：

- `app_main.c`
- `app_config.h/.c`
- `ctrl_bias.c`

## 4. 第一轮改造目标建议

不建议一上来追求“DPMZM 自动闭环”。第一轮更合适的目标是：

**先把工程改成 DPMZM 开环实验固件。**

对应能力应包括：

1. 三路偏压可独立设置: `I / Q / P`
2. 双导频模式可配置:
   - 外部导频
   - 板上导频
3. 支持三类扫描:
   - `I/Q MATP`
   - `P QTP`
   - `I/Q MITP`
4. 能通过 UART 把每个扫点的数据完整吐给 PC
5. 最终判点仍然交给上位机和人工，不急着放到 MCU 闭环里

这样可以最大化复用现有板卡、驱动、UART 和 Goertzel 内核，同时降低第一次改造的风险。

## 5. 文件级改造清单

下面按“保留 / 扩展 / 重构 / 新增”四类整理。

### 5.1 可以直接保留

这些模块建议尽量不动或者只做最小改动：

- `drivers/`
  - `drv_ads131m02.*`
  - `drv_dac8568.*`
  - `drv_board.*`
- `app/src/app_uart.c`
- `control/src/ctrl_pid.c`
- `dsp/src/dsp_goertzel.c`
- `dsp/src/dsp_pilot_gen.c`
- `cubemx/`

原因:

- 它们是板级和基础算法层
- 与 DPMZM 业务逻辑耦合较弱
- 风险最低，复用收益最高

### 5.2 需要扩展但不建议推倒重来

#### `control/inc/ctrl_modulator.h`

建议保留 strategy 总体思路，但要为 DPMZM 增加更明确的结构支持，例如：

- DPMZM 阶段枚举:
  - `MATP`
  - `QTP`
  - `MITP`
- DPMZM 目标通道枚举:
  - `I`
  - `Q`
  - `P`
- DPMZM 观测量结构:
  - `mag_fi`
  - `mag_fq`
  - `mag_fdiff`
  - `mag_fsum`
  - `dc`

原因:

- 现有 `harmonic_data_t` 是围绕单导频 MZM 设计的
- 直接拿它承载 DPMZM 会让接口语义越来越乱

#### `dsp/inc/dsp_types.h`

建议从“单导频固定宏”改成“多频检测配置”：

- 不再把系统唯一导频写死为 `DSP_PILOT_FREQ_HZ`
- 引入 DPMZM 扫描时的频点配置
- 保留 `64 kSPS / 1280点 / 10块` 这些当前已验证的系统时基

### 5.3 需要大改的模块

#### `app/inc/app_config.h` 和 `app/src/app_config.c`

建议改成 DPMZM 默认配置模型：

- 三路偏压 DAC 通道
- 双导频源模式
- 双导频频率
- 双导频幅度
- 默认扫描块数
- 默认 dump 模式

这一步是 DPMZM 落地的基础。

#### `app/src/app_main.c`

这将是未来改动最多的文件之一。

建议从当前的 MZM 应用层流程中剥离掉以下内容：

- `scan vpi`
- `cal bias`
- `scan harmonics`
- MZM 专用的锚点拟合与 affine 拟合

并改成 DPMZM 实验流程导向：

- `scan matp i`
- `scan matp q`
- `scan qtp p`
- `scan mitp i`
- `scan mitp q`
- `set bias i/q/p`
- `set pilot-src`
- `set pilot i/q`
- `set dump`

#### `control/src/ctrl_bias.c`

这部分不能继续假设自己只做：

- 单 pilot
- 单 bias 通道
- `H1/H2`

建议把它拆成两个层面：

1. 保留现有单通道 MZM 闭环控制器作为旧能力
2. 新增 DPMZM 开环扫描控制器或 DPMZM 测量包装层

也就是说，第一轮不要把这份文件强行改成“同时兼容一切”，否则风险会很高。

### 5.4 建议新增的文件

建议至少新增下面这些文件：

- `control/inc/ctrl_modulator_dpmzm.h`
- `control/src/ctrl_modulator_dpmzm.c`
- `control/inc/ctrl_scan_dpmzm.h`
- `control/src/ctrl_scan_dpmzm.c`

职责建议如下：

- `ctrl_modulator_dpmzm.*`
  - 定义 DPMZM 阶段判据
  - 定义各观测量的主指标
  - 负责把频率分量解释成 `MATP/QTP/MITP` 候选

- `ctrl_scan_dpmzm.*`
  - 负责执行具体扫描
  - 每步设置三路偏压
  - 调用 ADC 采集
  - 计算多频结果
  - 输出 UART 行

这样可以避免把所有 DPMZM 逻辑都继续堆进 `app_main.c`。

## 6. 第一轮建议命令集

为贴近真实实验，建议第一轮固件优先实现下面这些 UART 命令：

### 基础设置

- `status`
- `stop`
- `dac mid`
- `set bias i <V>`
- `set bias q <V>`
- `set bias p <V>`
- `set pilot-src ext|onboard`
- `set pilot i <freq_hz> <mVpp>`
- `set pilot q <freq_hz> <mVpp>`
- `set dump metrics|raw|both`

### 开环扫描

- `scan matp i <start> <stop> <step> [blocks]`
- `scan matp q <start> <stop> <step> [blocks]`
- `scan qtp p <start> <stop> <step> [blocks]`
- `scan mitp i <start> <stop> <step> [blocks]`
- `scan mitp q <start> <stop> <step> [blocks]`

### 数据输出建议

建议板上输出两类行：

- `DPMZMCSV,...`
- `DPMZMSUM,...`

必要时再加：

- `DPMZMRAW,...`

## 7. 推荐实施顺序

### 第一步: 建立 DPMZM 配置骨架

先改：

- `app_config.h/.c`
- UART 命令解析

目的:

- 三路偏压和双导频配置能表达出来

### 第二步: 打通 DPMZM 开环扫描数据链

再改：

- `ctrl_scan_dpmzm.*`
- `app_main.c`

目的:

- 板上能扫点
- 串口能吐出曲线
- PC 能离线复现曲线

### 第三步: 接入多频观测

再改：

- `dsp_types.h`
- `ctrl_bias.c` 或新的 DPMZM 测量层

目的:

- 同时提取 `fI / fQ / fdiff / fsum`

### 第四步: 新增 DPMZM 策略层

新增：

- `ctrl_modulator_dpmzm.*`

目的:

- 统一封装 `MATP/QTP/MITP` 的阶段判据

### 第五步: 再考虑闭环

只有当前四步完成并经过真实光链路验证后，才建议开始考虑：

- I/Q/P 三路自动闭环
- 板上候选点自动筛选
- 板上隐式解排除

## 8. 当前最值得优先保留的设计资产

后续我们做 DPMZM 时，最值得珍惜的不是“现成多调制器代码”，而是下面这些已经验证过的资产：

- 板级驱动稳定
- UART 主循环隔离 ISR 的结构是对的
- Goertzel 内核可复用
- PID 内核可复用
- MZM 这一路已经把“扫描 -> 校准 -> 运行”三层分工摸出来了

因此 DPMZM 的最佳策略不是“在 MZM 文件里一点点补丁”，而是：

**保留底层通用件，新增 DPMZM 业务层。**

## 9. 结论

当前工程确实给 DPMZM 留了接口，但这些接口主要体现在：

- 类型枚举
- strategy 结构
- 注册入口
- UART 入口
- 文档规划

它们说明“方向已经预留”，但还不能说明“DPMZM 只差一个策略文件”。

从代码真实结构来看，DPMZM 改造至少一定会涉及：

- `app_config`
- `app_main`
- `ctrl_bias`
- 新的 `ctrl_modulator_dpmzm`
- 可能新增 `ctrl_scan_dpmzm`

后续如果要稳定推进，建议把第一轮目标锁定为：

**DPMZM 开环实验固件，而不是直接追求自动闭环固件。**
