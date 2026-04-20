# Spec 06C - DPMZM 第一批创建文件清单

> 状态: Draft  
> 目标: 在不破坏原 `app_main.c` 和 `ctrl_bias.c` 的前提下，把 DPMZM 第一轮真正需要创建的文件收敛到最小集合。  
> 范围: 只面向 **DPMZM 开环实验版**，不包含三路自动闭环。  

## 1. 本文档解决什么问题

前两份文档已经明确了两件事：

- `spec-06a` 说明了当前工程里哪些接口是预留的，哪些地方其实还没实现。
- `spec-06b` 说明了后续应该走“平行新增文件，不破坏原 MZM 主线”的路线。

本文件再往前收一步，只回答一个问题：

> 如果现在马上开始动手，第一批到底先创建哪几个文件？

为了降低复杂度，第一批只保留 **7 个新文件**。

## 2. 第一批建议创建的 7 个文件

## 2.1 应用层

### 1. `app/inc/app_config_dpmzm.h`

作用:

- 定义 DPMZM 专用配置结构
- 把三路偏压和双导频参数独立出来

第一版必须包含的字段:

- `bias_i_dac_channel`
- `bias_q_dac_channel`
- `bias_p_dac_channel`
- `pilot_i_freq_hz`
- `pilot_q_freq_hz`
- `pilot_i_amp_v`
- `pilot_q_amp_v`
- `pilot_source`
- `scan_default_blocks`
- `dump_mode`

第一版不做:

- Flash 持久化
- 参数版本管理

### 2. `app/src/app_config_dpmzm.c`

作用:

- 提供 DPMZM 默认配置
- 提供一个获取全局 DPMZM 配置的接口

第一版必须实现:

- `app_config_dpmzm_defaults()`
- `app_config_dpmzm_get()`

第一版默认约束:

- 双导频默认由板上 DAC 生成，不再默认依赖外部信号源。
- `I/Q/P` 三路 DAC 通道可以先给出占位默认值，但必须明确标记为“待硬件接线确认”。

第一版不做:

- `save/load flash`

### 3. `app/inc/app_main_dpmzm.h`

作用:

- 作为 DPMZM 应用入口头文件
- 对外暴露 DPMZM 命令处理函数

第一版必须声明:

- `app_dpmzm_init()`
- `app_dpmzm_run()`
- `app_dpmzm_handle_command(const char *cmd)`

### 4. `app/src/app_main_dpmzm.c`

作用:

- 承载 DPMZM 的开环实验命令流
- 不复用原 `app_main.c` 的 MZM 标定流程

第一版建议只支持这些命令：

- `status`
- `set bias i <V>`
- `set bias q <V>`
- `set bias p <V>`
- `set pilot i <freq> <mVpp>`
- `set pilot q <freq> <mVpp>`
- `set dump metrics|raw|both`
- `scan matp i <start> <stop> <step> [blocks]`
- `scan matp q <start> <stop> <step> [blocks]`
- `scan qtp p <start> <stop> <step> [blocks]`
- `scan mitp i <start> <stop> <step> [blocks]`
- `scan mitp q <start> <stop> <step> [blocks]`

第一版不做:

- 自动状态机切换
- 闭环 start/stop/lock

## 2.2 控制层

### 5. `control/inc/ctrl_measure_dpmzm.h`

作用:

- 定义 DPMZM 单个扫点的多频观测结果结构

第一版必须包含的结构:

- `dpmzm_measurement_t`

建议字段:

- `mag_fi`
- `mag_fq`
- `mag_fdiff`
- `mag_fsum`
- `dc_mean`

这份头文件的目标很简单：

先把“DPMZM 一个扫点到底要产出什么数据”统一下来。

### 6. `control/src/ctrl_measure_dpmzm.c`

作用:

- 基于已有 `dsp_goertzel` 内核组织 4 个频点测量
- 不改原 `ctrl_bias.c`

第一版必须做的事情:

1. 初始化 4 个 Goertzel：
   - `fI`
   - `fQ`
   - `|fI-fQ|`
   - `fI+fQ`
2. 对一段 ADC 数据做多频提取
3. 输出 `dpmzm_measurement_t`

第一版不做:

- 相位解释
- 锁定判断
- 分支分类

### 7. `control/src/ctrl_scan_dpmzm.c`

作用:

- 负责 DPMZM 开环扫描执行
- 是第一轮里最核心的新增文件

第一版必须做的事情:

1. 设置 `I/Q/P` 三路偏压
2. 每个扫点调用 `ctrl_measure_dpmzm`
3. 根据扫描类型选择主观测量
4. 输出一行 CSV
5. 扫描结束输出一行 summary

第一版支持的扫描类型:

- `MATP-I`
- `MATP-Q`
- `QTP-P`
- `MITP-I`
- `MITP-Q`

第一版不做:

- 自动隐式解排除
- 自动 MATP/MITP 分支最终判决
- 自动闭环切换

## 3. 为什么第一批不先创建 `ctrl_modulator_dpmzm.*`

这一步是有意压后的。

原因是：

- 第一轮我们最需要的是“把曲线扫出来”
- 不是马上在板上完成复杂判据

所以第一批的重点应该是：

1. 配置能表达出来
2. 命令能发下去
3. 三路偏压能扫
4. 四个频点能量出来
5. 串口数据能吐出来

等这条链真正打通以后，再单独新增：

- `ctrl_modulator_dpmzm.h`
- `ctrl_modulator_dpmzm.c`

去承载更复杂的“候选点解释”和“阶段判据”。

## 4. 第一批文件之间的依赖关系

推荐依赖顺序如下：

```text
app_main_dpmzm
  -> app_config_dpmzm
  -> ctrl_scan_dpmzm
       -> ctrl_measure_dpmzm
            -> dsp_goertzel / dsp_pilot_gen / drivers
```

也就是说：

- `app_main_dpmzm` 只负责命令和流程
- `ctrl_scan_dpmzm` 负责真正扫点
- `ctrl_measure_dpmzm` 负责单点观测

这样职责最清楚，也最不容易把新逻辑再堆回应用层。

## 5. 第一批每个文件的“完成标准”

### `app_config_dpmzm.*`

完成标准:

- 可以拿到一份默认 DPMZM 配置
- 配置里包含三路偏压和双导频参数

### `app_main_dpmzm.*`

完成标准:

- 能正确解析 DPMZM 基础命令
- 能触发 5 类扫描入口

### `ctrl_measure_dpmzm.*`

完成标准:

- 输入一段 ADC 数据
- 能稳定输出四个目标频点的幅值

### `ctrl_scan_dpmzm.c`

完成标准:

- 能独立跑完一次扫描
- 能吐出曲线数据
- 能给出 summary

## 6. 第一批之后的下一步

只有当这 7 个文件完成并且真实实验能跑通后，才建议进入下一步：

### 第二批再新增

- `control/inc/ctrl_modulator_dpmzm.h`
- `control/src/ctrl_modulator_dpmzm.c`
- `docs/uart_dpmzm.md`
- `test/test_dpmzm_multifreq.c`
- `test/test_dpmzm_summary.c`

第二批主要解决的是：

- 板上判据抽象
- 文档固化
- 主机测试补齐

## 7. 结论

如果现在就开始动手，第一批最值得先创建的不是一大堆文件，而是下面这 7 个：

1. `app/inc/app_config_dpmzm.h`
2. `app/src/app_config_dpmzm.c`
3. `app/inc/app_main_dpmzm.h`
4. `app/src/app_main_dpmzm.c`
5. `control/inc/ctrl_measure_dpmzm.h`
6. `control/src/ctrl_measure_dpmzm.c`
7. `control/src/ctrl_scan_dpmzm.c`

这 7 个文件已经足够支撑：

- DPMZM 三路偏压表达
- 双导频多频测量
- `MATP/QTP/MITP` 开环扫描
- 串口数据输出

而且不会破坏原有 MZM 主线。
