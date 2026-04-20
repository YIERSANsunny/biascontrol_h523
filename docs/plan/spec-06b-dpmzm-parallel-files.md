# Spec 06B - DPMZM 并行新增文件清单

> 状态: Draft  
> 目标: 在**不破坏原有 `ctrl_bias.c` 和 `app_main.c` 的 MZM 代码逻辑**前提下，为 DPMZM 新增一套平行文件结构。  
> 约束: 原 MZM 主线继续保留并可单独编译、单独运行。DPMZM 作为并行实现逐步接入。  

## 1. 设计原则

本阶段默认采用下面这条原则：

> 保留原有 `app_main.c`、`ctrl_bias.c`、`ctrl_modulator_mzm.c` 作为 MZM 主线，不在原文件中硬塞 DPMZM 业务逻辑；DPMZM 通过新增平行模块实现。

这样做的好处有三点：

1. 原来的 MZM 逻辑不会被改坏。
2. DPMZM 可以按自己的扫描流程组织代码，不受单路 MZM 框架限制。
3. 后续可以保留两个编译入口或两个 target，便于对比和回退。

## 2. 建议保留不动的原始文件

以下文件建议继续作为 MZM 参考主线保留：

### 应用层

- `app/src/app_main.c`
- `app/inc/app_main.h`
- `app/src/app_config.c`
- `app/inc/app_config.h`
- `app/src/app_uart.c`

### 控制层

- `control/src/ctrl_bias.c`
- `control/inc/ctrl_bias.h`
- `control/src/ctrl_modulator_mzm.c`
- `control/inc/ctrl_modulator_mzm.h`
- `control/src/ctrl_pid.c`
- `control/inc/ctrl_pid.h`

### DSP 与驱动层

- `dsp/src/dsp_goertzel.c`
- `dsp/inc/dsp_goertzel.h`
- `dsp/src/dsp_pilot_gen.c`
- `dsp/inc/dsp_pilot_gen.h`
- `drivers/` 全部
- `cubemx/` 全部

这些文件是当前工程最稳定、最值得复用的部分。

## 3. 建议新增的 DPMZM 文件

下面的文件按“最小能跑通开环实验”的目标来设计。

## 3.1 应用层新增文件

### `app/inc/app_main_dpmzm.h`

职责:

- 定义 DPMZM 应用入口
- 暴露 DPMZM 命令处理接口
- 与原 `app_main.h` 平行存在

建议内容:

- `app_dpmzm_init()`
- `app_dpmzm_run()`
- `app_dpmzm_handle_command()`

### `app/src/app_main_dpmzm.c`

职责:

- 承载 DPMZM 的应用状态机
- 组织 DPMZM 的串口命令
- 串起 `MATP -> QTP -> MITP` 的开环实验流程

建议不要复刻原 MZM 的 `scan vpi / cal bias / scan harmonics` 逻辑，而是围绕 DPMZM 新流程组织：

- `set bias i/q/p`
- `set pilot-src`
- `set pilot i/q`
- `set dump`
- `scan matp i/q`
- `scan qtp p`
- `scan mitp i/q`

当前实验约束补充:

- DPMZM 的双导频应由板上 DAC 路径生成。
- `external` 可以保留在配置枚举或文档层作为预留字样，但当前实现阶段不应作为有效工作模式。

### `app/inc/app_config_dpmzm.h`

职责:

- 定义 DPMZM 专用配置结构
- 与原 `app_config.h` 的单通道配置分离

建议字段:

- `bias_i_dac_channel`
- `bias_q_dac_channel`
- `bias_p_dac_channel`
- `pilot_source_mode`
- `pilot_i_freq_hz`
- `pilot_q_freq_hz`
- `pilot_i_amplitude_v`
- `pilot_q_amplitude_v`
- `scan_default_blocks`
- `dump_mode`

补充说明:

- 原规划文档只说明 DPMZM 需要三路偏压通道，但**没有冻结 I/Q/P 分别使用哪一个 DAC 物理通道**。
- 因此后续代码里的 `bias_i_dac_channel / bias_q_dac_channel / bias_p_dac_channel` 只能先给出“默认值”，不能把某个通道映射误写成已确认结论。

### `app/src/app_config_dpmzm.c`

职责:

- 提供 DPMZM 默认配置
- 预留后续 Flash 持久化接口

## 3.2 控制层新增文件

### `control/inc/ctrl_modulator_dpmzm.h`

职责:

- 定义 DPMZM 专用类型
- 定义 DPMZM 阶段与目标通道枚举
- 提供 DPMZM 策略实例导出接口

建议内容:

- `dpmzm_stage_t`
  - `DPMZM_STAGE_MATP`
  - `DPMZM_STAGE_QTP`
  - `DPMZM_STAGE_MITP`
- `dpmzm_target_t`
  - `DPMZM_TARGET_I`
  - `DPMZM_TARGET_Q`
  - `DPMZM_TARGET_P`
- `dpmzm_get_strategy()`

### `control/src/ctrl_modulator_dpmzm.c`

职责:

- 承载 DPMZM 的阶段判据
- 负责把多频观测量解释为 `MATP/QTP/MITP` 候选
- 与 `ctrl_modulator_mzm.c` 平行

首版建议不要做自动闭环控制，只做：

- 主指标选择
- 候选点归类
- 扫描摘要输出

例如：

- `MATP` 看 `fI` 或 `fQ`
- `QTP` 主看 `fI + fQ`
- `MITP` 看对应单路一阶分量，但保留 MATP/MITP 双分支判断信息

### `control/inc/ctrl_scan_dpmzm.h`

职责:

- 定义 DPMZM 扫描任务结构
- 定义扫点结果结构

建议内容:

- 扫描范围
- 步进
- 当前阶段
- 当前目标通道
- 固定偏压
- 输出模式

### `control/src/ctrl_scan_dpmzm.c`

职责:

- 执行 DPMZM 开环扫描
- 每一步设置 I/Q/P 三路偏压
- 触发采样
- 组织多频结果
- 吐出串口 CSV / summary

这份文件会是 DPMZM 开环实验的主执行层。

### `control/inc/ctrl_measure_dpmzm.h`

职责:

- 抽象 DPMZM 的测量结果
- 封装多频幅值结果

建议结构:

- `mag_fi`
- `mag_fq`
- `mag_fdiff`
- `mag_fsum`
- `dc`

### `control/src/ctrl_measure_dpmzm.c`

职责:

- 基于现有 `dsp_goertzel` 内核组织 4 个频点的测量
- 不修改原 `ctrl_bias.c`
- 单独作为 DPMZM 观测层使用

## 3.3 文档与测试层新增文件

### `docs/uart_dpmzm.md`

职责:

- 记录 DPMZM 新命令集
- 记录每类 CSV 输出字段

### `docs/bringup_dpmzm.md`

职责:

- 记录 DPMZM 板级 bring-up 顺序
- 记录外部导频/板上导频的实验切换流程

### `test/test_dpmzm_multifreq.c`

职责:

- 验证 `fI / fQ / |fI-fQ| / fI+fQ` 四频测量是否正确

### `test/test_dpmzm_summary.c`

职责:

- 验证 `MATP/QTP/MITP` 扫描摘要逻辑是否正确

## 4. 最小可运行文件集合

如果只追求“第一版 DPMZM 开环实验能跑起来”，建议第一轮至少新增下面这些文件：

### 必要文件

- `app/inc/app_main_dpmzm.h`
- `app/src/app_main_dpmzm.c`
- `app/inc/app_config_dpmzm.h`
- `app/src/app_config_dpmzm.c`
- `control/inc/ctrl_modulator_dpmzm.h`
- `control/src/ctrl_modulator_dpmzm.c`
- `control/inc/ctrl_scan_dpmzm.h`
- `control/src/ctrl_scan_dpmzm.c`
- `control/inc/ctrl_measure_dpmzm.h`
- `control/src/ctrl_measure_dpmzm.c`

### 推荐同步新增

- `docs/uart_dpmzm.md`
- `docs/bringup_dpmzm.md`
- `test/test_dpmzm_multifreq.c`
- `test/test_dpmzm_summary.c`

## 5. 推荐的编译接入方式

为了不破坏原 MZM 主线，推荐下面两种接入方式中的一种。

### 方案 A: 新增独立 target

示意:

- 原 target: `biascontrol.elf`
- 新 target: `biascontrol_dpmzm.elf`

优点:

- 原 MZM 固件不受影响
- DPMZM 可以单独选择源文件集合
- 最适合当前“并行维护”阶段

### 方案 B: 用编译选项选择入口

示意:

- `-DAPP_VARIANT_MZM`
- `-DAPP_VARIANT_DPMZM`

然后在 `CMakeLists.txt` 中切换：

- 编译 `app_main.c`
- 或编译 `app_main_dpmzm.c`

优点:

- 工程表面更统一

缺点:

- 早期阶段更容易把两个入口缠在一起

**当前更推荐方案 A。**

## 6. 第一轮不建议做的事情

为了避免把问题复杂化，第一轮不建议做下面这些事：

- 不要在 `ctrl_bias.c` 里硬塞 DPMZM 多频逻辑
- 不要在 `app_main.c` 里继续堆新的 `scan matp/qtp/mitp` 分支
- 不要第一轮就做三路自动闭环
- 不要第一轮就做板上隐式解自动排除
- 不要第一轮就尝试让 MZM 与 DPMZM 共用完全相同的数据结构

## 7. 推荐的第一轮实现目标

第一轮目标建议定为：

**DPMZM 开环实验版固件**

验收标准：

1. 能独立设置 `I/Q/P` 三路偏压
2. 能处理双导频配置
3. 能完成下列扫描命令：
   - `scan matp i`
   - `scan matp q`
   - `scan qtp p`
   - `scan mitp i`
   - `scan mitp q`
4. 能通过串口吐出曲线数据
5. 能被 PC 侧脚本直接消费

## 8. 结论

如果师兄的要求是：

> 保留原 `ctrl_bias.c` 和 `app_main.c` 的代码逻辑

那么最合理的实现路径不是“在原文件里继续加条件分支”，而是：

**保留 MZM 主线，新增 DPMZM 平行文件。**

这套新增文件结构既能满足“不破坏原逻辑”的要求，也能给后续 DPMZM 扩展留下足够干净的生长空间。
