# Spec 06D - DPMZM 功能总览与开发状态
> 状态: Active working note  
> 作用: 用一份文档回答 4 个问题  
> 1. DPMZM 这条线最终要做什么  
> 2. 现在已经做到了什么  
> 3. 还缺哪些关键能力  
> 4. 下一步应该优先做什么

---

## 1. 文档定位

这份文档不是理论文档，也不是底层驱动文档。

它的定位是:

- 作为 **DPMZM 代码工作的总入口**
- 作为后续查找文件的 **导航页**
- 作为判断“当前做到哪一步”的 **阶段状态页**

后续如果继续推进 DPMZM，建议优先阅读顺序是:

1. `spec-06d-dpmzm-status-overview.md`
2. `spec-06a-dpmzm-migration-checklist.md`
3. `spec-06b-dpmzm-parallel-files.md`
4. `spec-06c-dpmzm-first-batch-files.md`

---

## 2. DPMZM 最终想实现什么

当前 DPMZM 路线的目标，不是简单把旧 MZM 固件改成“三路版本”，而是建立一套适用于 DPMZM 的偏压控制链路。

### 2.1 近期目标: 开环实验固件

先实现一套 **DPMZM 开环实验版固件**，支持:

- 三路偏压: `I / Q / P`
- 双导频: `fI / fQ`
- 多频检测:
  - `fI`
  - `fQ`
  - `|fI-fQ|`
  - `fI+fQ`
- 三类扫描:
  - `I/Q MATP`
  - `P QTP`
  - `I/Q MITP`
- 串口输出统一的扫描数据格式

这一阶段的目标是:

- 把曲线扫出来
- 把候选点测出来
- 让 PC 侧脚本能够直接消费这些数据

### 2.2 中期目标: 板上判据与候选点解释

在开环扫描跑通之后，再增加板上解释层，例如:

- MATP 候选识别
- QTP 主判据与辅判据选择
- MITP 分支识别
- 隐式解与主解的分类

### 2.3 远期目标: 自动闭环控制

最终才进入真正的 DPMZM 自动控制阶段，例如:

- 三路顺序锁定
- 多阶段偏压切换
- 板上自动寻找并锁定:
  - `I-MATP`
  - `Q-MATP`
  - `P-QTP`
  - `I-MITP`
  - `Q-MITP`

---

## 3. 当前开发原则

当前 DPMZM 代码路线有一个很重要的约束:

**不破坏原有 MZM 主线逻辑。**

更具体地说:

- 原 `app_main.c` 保持为 MZM 主入口
- 原 `ctrl_bias.c` 保持为 MZM 控制主循环
- 原 `ctrl_modulator_mzm.c` 保持为 MZM 策略实现

DPMZM 通过 **平行新增文件** 的方式推进，而不是在原 MZM 文件里硬塞新逻辑。

这条原则的目的很明确:

- 不把已经验证过的 MZM 逻辑改坏
- DPMZM 可以单独演进
- 后续可以并行保留 MZM 与 DPMZM 两条路径

---

## 4. 当前已经实现的内容

这一节只记录 **已经落到代码里的功能**。

### 4.1 DPMZM 独立配置层

已经完成:

- 独立的 DPMZM 配置结构
- 三路偏压配置:
  - `bias_i_dac_channel`
  - `bias_q_dac_channel`
  - `bias_p_dac_channel`
- 双导频配置:
  - `pilot_i_freq_hz`
  - `pilot_q_freq_hz`
  - `pilot_i_amp_v`
  - `pilot_q_amp_v`
- 导频源配置:
  - `pilot_source`
- 扫描缺省配置:
  - `scan_default_blocks`
  - `dump_mode`

对应文件:

- `app/inc/app_config_dpmzm.h`
- `app/src/app_config_dpmzm.c`

当前约定:

- 默认导频源是 **板上 DAC 生成**
- `I/Q/P` 对应哪个 DAC 物理通道仍是 **占位默认值**，还没有按真实接线定死

### 4.2 DPMZM 独立应用入口

已经完成:

- DPMZM 独立上下文
- DPMZM 独立命令入口
- 基础状态输出
- 三路偏压设置命令
- 双导频参数设置命令
- 输出模式设置命令
- 三类扫描命令入口

当前已支持命令:

- `status`
- `set bias i <V>`
- `set bias q <V>`
- `set bias p <V>`
- `set pilot-src onboard`
- `set pilot i <freq_hz> <mVpp>`
- `set pilot q <freq_hz> <mVpp>`
- `set dump metrics|raw|both`
- `scan matp i ...`
- `scan matp q ...`
- `scan qtp p ...`
- `scan mitp i ...`
- `scan mitp q ...`

对应文件:

- `app/inc/app_main_dpmzm.h`
- `app/src/app_main_dpmzm.c`

### 4.3 DPMZM 多频测量层

已经完成:

- 单点多频测量抽象
- 同时提取:
  - `mag_fi`
  - `mag_fq`
  - `mag_fdiff`
  - `mag_fsum`
  - `dc_mean`
- 支持整块数据一次性测量
- 支持样本逐点喂入

对应文件:

- `control/inc/ctrl_measure_dpmzm.h`
- `control/src/ctrl_measure_dpmzm.c`

### 4.4 DPMZM 开环扫描层

已经完成:

- DPMZM 独立扫描请求结构
- 扫描阶段枚举:
  - `MATP`
  - `QTP`
  - `MITP`
- 扫描目标枚举:
  - `I`
  - `Q`
  - `P`
- 扫点执行流程:
  - 设置三路偏压
  - 板上生成 `I/Q` 双导频
  - 等待 `DRDY`
  - 读取 ADS131M02
  - 进行四频点 Goertzel
  - 计算单点结果
  - 输出串口结果
- 扫描总结输出

当前输出格式已经实现:

- `DPMZMCSV`
- `DPMZMRAW`
- `DPMZMSUM`

当前主判据已经确定:

- `MATP / MITP`
  - 扫 `I` 时主看 `mag_fI`
  - 扫 `Q` 时主看 `mag_fQ`
- `QTP`
  - 主看 `mag_fsum`
  - 辅量仍会输出 `mag_fdiff`

对应文件:

- `control/inc/ctrl_scan_dpmzm.h`
- `control/src/ctrl_scan_dpmzm.c`

### 4.5 主机侧测试

已经完成:

- 一个 DPMZM 多频测量测试
- 验证四个频点:
  - `1000 Hz`
  - `1200 Hz`
  - `200 Hz`
  - `2200 Hz`
- 验证 `dc_mean`
- 验证 `sample_count`

对应文件:

- `test/test_dpmzm_multifreq.c`

对应构建入口:

- `CMakeLists.txt`

---

## 5. 当前还没有实现的内容

这一节记录 **还没有完成，或者虽然预留但还不能真正用** 的部分。

### 5.1 还没有接入原主串口分发链路

当前状态:

- `app_main_dpmzm.c` 已经能单独处理命令
- 但它还没有接进原有 `app_uart + app_main` 主路径

这意味着:

- DPMZM 命令入口存在
- 但还不是当前老工程默认启动后的正式入口

### 5.2 还没有独立的 DPMZM 板上判据层

当前状态:

- 目前只有“测量”和“扫描”
- 还没有更高层的 `ctrl_modulator_dpmzm.*`

这意味着:

- 板上已经能输出曲线数据
- 但还没有形成完整的“候选点解释层”

尚未完成的内容包括:

- MATP 主解/隐式解区分
- QTP 主判据/辅判据融合
- MITP 分支自动区分
- 真实工作点分类

### 5.3 还没有自动闭环

当前状态:

- 现在只支持开环扫描
- 不支持自动锁点

尚未完成的内容包括:

- 自动阶段切换
- 自动寻找并锁定三路工作点
- 闭环 PID 结构的 DPMZM 化

### 5.4 还没有最终硬件通道映射

当前状态:

- `I / Q / P` 的 DAC 通道只是暂时占位
- 还没有和真实接线固定绑定

这意味着:

- 代码骨架已经具备
- 但还要等真实实验接线再冻结映射

### 5.5 还没有高实时板上导频引擎

当前状态:

- 当前实现是阻塞式扫描
- 扫描时由前台代码逐点更新 `I/Q` DAC 输出

这版实现的意义是:

- 先把 DPMZM 开环链路打通
- 先不为了“最终高实时”而复杂化第一版

后续如果需要提高实时性，可能会进入:

- 定时器驱动
- DMA 驱动
- 更稳定的板上导频输出引擎

### 5.6 还没有扫描总结的主机测试

当前状态:

- 四频点提取测试已经有了
- 但“扫描后如何选出 best point”的摘要测试还没有补

建议后续增加:

- `test_dpmzm_scan_summary.c`

---

## 6. 当前代码地图

这一节只回答一个问题:

**如果后面要继续做 DPMZM，先看哪些文件。**

### 6.1 DPMZM 核心入口

- `app/inc/app_config_dpmzm.h`
- `app/src/app_config_dpmzm.c`
- `app/inc/app_main_dpmzm.h`
- `app/src/app_main_dpmzm.c`

### 6.2 DPMZM 测量与扫描

- `control/inc/ctrl_measure_dpmzm.h`
- `control/src/ctrl_measure_dpmzm.c`
- `control/inc/ctrl_scan_dpmzm.h`
- `control/src/ctrl_scan_dpmzm.c`

### 6.3 可直接复用的旧工程通用层

- `drivers/inc/drv_board.h`
- `drivers/inc/drv_dac8568.h`
- `drivers/inc/drv_ads131m02.h`
- `dsp/inc/dsp_goertzel.h`
- `dsp/inc/dsp_types.h`

### 6.4 当前不要破坏的 MZM 主线

- `app/src/app_main.c`
- `control/src/ctrl_bias.c`
- `control/src/ctrl_modulator_mzm.c`

---

## 7. 当前我们到底做到哪一步

如果按阶段看，DPMZM 当前已经走到:

### 已完成

- DPMZM 配置层
- DPMZM 命令层
- DPMZM 四频点测量层
- DPMZM 开环扫描层
- DPMZM 多频测试

### 正在做

- 把 DPMZM 扫描链路从“平行骨架”逐步接成可实际实验使用的路径

### 还没做

- 主入口接线
- 板上候选点解释层
- 板上自动闭环
- 最终硬件通道冻结
- 高实时导频引擎

---

## 8. 下一步推荐顺序

为了避免开发路径发散，建议后续按下面的顺序推进。

### 第 1 步

把 `app_main_dpmzm` 接到实际串口入口。

目标:

- 让 DPMZM 命令从“平行代码”变成“可真实调用”

### 第 2 步

补 `scan summary` 相关测试。

目标:

- 验证 `DPMZMSUM` 的候选点提取逻辑

### 第 3 步

冻结真实硬件 `I/Q/P` 通道映射。

目标:

- 让当前扫描层可以直接对真实 DPMZM 实验链路工作

### 第 4 步

新增 `ctrl_modulator_dpmzm.*`

目标:

- 在板上增加候选点解释与阶段判据抽象

### 第 5 步

根据实验反馈决定是否把板上双导频输出从前台阻塞式实现，升级成定时器/DMA 驱动。

---

## 9. 一句话总结

当前 DPMZM 代码已经不是“只有想法”的阶段了。

现在已经具备:

- 三路偏压配置
- 双导频配置
- 四频点测量
- `MATP / QTP / MITP` 开环扫描
- 统一串口输出格式

但它仍然处于 **DPMZM 开环实验版骨架** 阶段，还没有进入:

- 原工程正式主入口
- 板上候选点自动解释
- 三路自动闭环控制

后续继续推进时，建议把这份文档作为 DPMZM 代码工作的默认总入口。
