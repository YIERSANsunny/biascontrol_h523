# Spec 06G - DPMZM 第一版固件实现清单

> 状态：**V1 COMPLETE / 阶段性完成**
> 日期：2026-04-28
> 用途：记录 DPMZM 第一版固件“板上自动粗捕获”的实现范围、完成情况、验证结果，以及进入第二版细扫前的交接点。
> 结论：第一版固件目标已经完成，可以进入第二版“自动细扫与局部收敛”阶段。

---

## 1. 第一版要解决的问题

第一版固件的目标不是直接实现最终闭环，而是先把人工开环找点流程固化到板上，让单片机能够自动完成一次粗捕获。

第一版回答的问题是：

- 板子能不能自动扫出 `I/Q-MATP` 的参考点？
- 板子能不能根据 `MATP` 结果给 `P-QTP` 提供可用初值？
- 板子能不能自动扫出 `P-QTP` 粗正交点？
- 板子能不能继续扫出 `I/Q-MITP` 粗最小点？
- 扫描结果是否和人工读图得到的点位大体一致？

现在这些目标已经完成，并且最新一次自动扫描结果和我们人工找点结果基本一致。

---

## 2. 第一版范围边界

### 2.1 第一版已经完成的功能

第一版固件已经实现：

- `dpmzm auto coarse` 一条命令触发完整自动粗捕获。
- 自动执行 `I-MATP` 粗扫。
- 自动执行 `Q-MATP` 粗扫。
- 自动提取 `I/Q-MATP` 候选点。
- 根据 `MATP` 谷底之间的一阶功率高平台，选择 `P-QTP` 初始工作区。
- 自动设置 `I/Q` 到 `P-QTP` 初始偏压。
- 自动执行 `P-QTP` 粗扫，只使用 `fI+fQ = 2200 Hz` 作为主判据。
- 自动设置 `P` 到粗正交点。
- 自动执行 `I-MITP` 粗扫。
- 自动执行 `Q-MITP` 粗扫。
- 输出最终粗捕获结果，并把 `I/Q/P` 偏压应用到板子上。
- 串口输出完整 `DPMZMCSV`，便于保存、解析和画图验证。

一句话总结：

- **第一版 = 板上自动粗捕获，已经完成。**

### 2.2 第一版刻意不做的事情

第一版暂时不做：

- 自动细扫。
- 小窗口重复收敛。
- 多轮迭代优化。
- 真正闭环的小扰动锁定。
- 长时间漂移跟踪。
- 多分支全局最优搜索。

这些内容放到第二版和第三版。

---

## 3. 第一版关键修复与实现结果

### 3.1 连续导频

第一版已经从“扫描采样时临时生成导频”改为：

- 导频由 `pilot-open on/off` 控制。
- 扫描命令不再负责临时打开/关闭导频。
- `dpmzm auto coarse` 在当前导频状态基础上执行扫描。

这避免了扫描内部导频状态反复切换带来的不确定性。

### 3.2 TIM6 导频频率修正

之前发现 raw 频谱强峰不在 `1000/1200 Hz`，而偏到约 `960/1150 Hz`。根因是 TIM6 实际更新率与代码假设不一致。

已经修正后：

- ADC raw FFT 中 `fI` 回到 `1000 Hz`。
- ADC raw FFT 中 `fQ` 回到 `1200 Hz`。
- 固件 Goertzel 盯住的频点与真实导频频点一致。

### 3.3 扫描测量改为“先采样、后计算”

这是第一版里最关键的修复。

旧逻辑：

```text
每来一个 ADC 点 -> 立刻跑多个 Goertzel -> 继续等下一个点
```

问题：

- 前台计算负担会扰乱采样节奏。
- scan 中得到的一阶功率明显偏小。
- `I/Q=0 V` 时，raw FFT 与 scan CSV 不一致。

新逻辑：

```text
每个 block 先连续采满 N 点到 RAM 缓冲区
采样完成后再离线执行 Goertzel / 打印 RAW
```

修复后验证：

- raw FFT 的 `fI=-0.92 dBm`，scan I=0 得到约 `-0.93 dBm`。
- raw FFT 的 `fQ=-5.87 dBm`，scan Q=0 得到约 `-5.88 dBm`。
- raw FFT 的 `fI+fQ=-58.65 dBm`，scan P/QTP=0 得到约 `-59.1 dBm`。

这说明扫描路径已经和原始 ADC 频谱对齐。

### 3.4 按扫描阶段只计算必要频点

第一版现在按阶段选择测量频点：

| 扫描阶段 | 主判据 | 固件计算 |
|---|---:|---|
| `I-MATP` / `I-MITP` | `fI = 1000 Hz` | 只计算 `mag_fI` 和 `dc_mean` |
| `Q-MATP` / `Q-MITP` | `fQ = 1200 Hz` | 只计算 `mag_fQ` 和 `dc_mean` |
| `P-QTP` | `fI+fQ = 2200 Hz` | 只计算 `mag_fsum` 和 `dc_mean` |

说明：

- `200 Hz = |fI-fQ|` 受低频干扰明显，第一版不再作为自动控制判据。
- CSV 列格式保持不变，未计算的频点填 `0.0`，避免破坏脚本兼容性。

---

## 4. 第一版当前板上流程

当前 `dpmzm auto coarse` 的流程是：

1. 执行 `I-MATP` 粗扫。
2. 从 `I-MATP` 曲线中提取候选谷底。
3. 执行 `Q-MATP` 粗扫。
4. 从 `Q-MATP` 曲线中提取候选谷底。
5. 根据相邻 `MATP` 谷底之间的一阶功率高平台，选择 `P-QTP` 初始 `I/Q` 工作区。
6. 设置 `I/Q` 到 `P-QTP` 初始偏压。
7. 执行 `P-QTP` 粗扫。
8. 设置 `P` 到粗正交点。
9. 执行 `I-MITP` 粗扫。
10. 设置 `I` 到粗最小点。
11. 执行 `Q-MITP` 粗扫。
12. 设置 `Q` 到粗最小点。
13. 输出自动粗捕获结果。
14. 将最终 `I/Q/P` 粗结果应用到板子。

---

## 5. 第一版最新验证结果

最新一次自动扫描：

```text
command: dpmzm auto coarse
date:    2026-04-28
points:  905 total, 181 points per segment
```

固件输出结果：

```text
error:         OK
I MATP ref:    +7.900V
Q MATP ref:    +1.900V
I P-init:      +2.000V
Q P-init:      -3.250V
P QTP coarse:  +3.000V (valid)
I MITP coarse: +4.400V (valid)
Q MITP coarse: -5.800V (valid)
applied:       I=+4.400V Q=-5.800V P=+3.000V
```

分段扫描统计：

| 段落 | 点数 | 固件选点 | 对应功率 |
|---|---:|---:|---:|
| `I-MATP` | 181 | `+7.900 V` | `-37.17 dBm` |
| `Q-MATP` | 181 | `+1.900 V` | `-32.81 dBm` |
| `P-QTP` | 181 | `+3.000 V` | `-64.04 dBm` |
| `I-MITP` | 181 | `+4.400 V` | `-39.27 dBm` |
| `Q-MITP` | 181 | `-5.800 V` | `-57.96 dBm` |

结论：

- 自动粗捕获结果已经和人工找点基本一致。
- 第一版固件可以视为完成。
- 后续不应该继续在第一版粗扫上打磨过久，应该进入第二版细扫。

---

## 6. 第一版代码落点

第一版核心代码文件：

- `control/inc/ctrl_auto_dpmzm.h`
- `control/src/ctrl_auto_dpmzm.c`
- `control/inc/ctrl_scan_dpmzm.h`
- `control/src/ctrl_scan_dpmzm.c`
- `control/inc/ctrl_measure_dpmzm.h`
- `control/src/ctrl_measure_dpmzm.c`
- `app/src/app_main_dpmzm.c`
- `drivers/inc/drv_board.h`
- `drivers/src/drv_board.c`

第一版新增/稳定的串口接口：

```text
dpmzm auto coarse
dpmzm set pilot-open on
dpmzm set pilot-open off
dpmzm set dump metrics
dpmzm capture raw <N> <settle_ms>
```

---

## 7. 第一版完成标志

第一版完成条件如下：

1. 板上可以通过一条命令完成自动粗捕获。
2. 自动粗捕获可以稳定输出 `I/Q/P` 三路粗候选点。
3. 扫描功率与 ADC raw FFT 一致，不再出现 scan 功率大幅偏小的问题。
4. 自动粗捕获结果和人工开环找点结果大体一致。
5. 出错时能返回明确错误状态。
6. 扫描结果可以通过串口保存、解析并画图验证。

当前状态：

- **以上条件均已满足。**

---

## 8. 第二版目标：自动细扫

第一版完成后，第二版要做的是：

- 在第一版粗捕获结果附近执行小范围细扫。
- 用更小步进重新寻找 `P-QTP`、`I-MITP`、`Q-MITP`。
- 将粗扫结果变成更接近人工精扫结果的精确偏压。
- 为第三版闭环锁定提供更可靠的初始点。

建议第二版先实现固定顺序：

1. 以第一版 `P QTP coarse` 为中心，对 `P` 做细扫。
2. 设置 `P` 到细扫后的 `QTP`。
3. 以第一版 `I MITP coarse` 为中心，对 `I` 做细扫。
4. 设置 `I` 到细扫后的 `MITP`。
5. 以第一版 `Q MITP coarse` 为中心，对 `Q` 做细扫。
6. 设置 `Q` 到细扫后的 `MITP`。
7. 必要时再重复一轮 `P -> I -> Q` 小窗口细扫。

---

## 9. 第二版建议参数

第一版粗扫参数：

```text
range: -9.0 V to +9.0 V
step:  0.1 V
blocks: default scan blocks
```

第二版建议拆成两级：

- 第一级：宽窗细化，也可以叫中扫，用来把粗扫结果拉到更可靠的局部谷底附近。
- 第二级：小窗口细扫，用来在第一级结果附近做精确定位。

原因是第一版粗扫步进为 `0.1 V`，再加上 `MATP -> P-QTP -> MITP` 之间存在相互耦合，粗扫点只能保证“落在可用区域”，不应该假设它已经非常接近最终精确点。

### 9.1 第一级：宽窗细化

第一遍细扫使用较大的窗口和中等步进：

```text
P-QTP wide refine:
  center = p_qtp_coarse
  range  = center +/- 2.0 V
  step   = 0.05 V

I-MITP wide refine:
  center = i_mitp_coarse
  range  = center +/- 2.0 V
  step   = 0.05 V

Q-MITP wide refine:
  center = q_mitp_coarse
  range  = center +/- 2.0 V
  step   = 0.05 V
```

第一级的目标不是最终精确，而是得到：

```text
p_qtp_wide_best
i_mitp_wide_best
q_mitp_wide_best
```

后续小窗口细扫应该以这些 wide best 为中心，而不是继续以 coarse point 为中心。

### 9.2 第二级：小窗口细扫

```text
P-QTP fine:
  center = p_qtp_wide_best
  range  = center +/- 0.6 V
  step   = 0.01 V

I-MITP fine:
  center = i_mitp_wide_best
  range  = center +/- 0.3 V to +/- 0.6 V
  step   = 0.01 V

Q-MITP fine:
  center = q_mitp_wide_best
  range  = center +/- 0.3 V to +/- 0.6 V
  step   = 0.01 V
```

### 9.3 贴边后的追踪扩窗规则

如果第二级小窗口细扫的 best point 落在窗口边缘附近，说明真实谷底大概率还在窗口外侧。此时不要继续以旧 center 原地扩窗，而应该：

1. 将当前扫描通道偏压设置到这个边缘 best point。
2. 以这个边缘 best point 作为新的 center。
3. 执行 expanded fine scan。
4. 如果 expanded fine scan 仍然贴边，则最多再扩一次，或者返回 `NEED_WIDER_WINDOW`。

建议边缘判定：

```text
edge_margin = max(2 * step, 0.05 V)
```

示例：

```text
I fine:
  old center = 5.4 V
  range      = +/- 0.3 V
  scan       = 5.1 V to 5.7 V
  best       = 5.7 V

because best is near right edge:
  set I = 5.7 V
  I expanded fine:
    center = 5.7 V
    range  = +/- 0.6 V or +/- 1.0 V
    step   = 0.01 V
```

这样做的好处是：

- 搜索中心会跟着真实谷底方向移动。
- 不会浪费一半窗口在已经确认不是最优的方向。
- 对 `P/I/Q` 三路耦合更友好，因为每一步都会先把当前通道设置到当前最佳点。
- 如果谷底继续往外，算法也能连续追出去，而不是卡在旧中心附近。

---

## 10. 第二版建议命令接口

建议新增：

```text
dpmzm auto fine
```

作用：

- 基于最近一次 `auto coarse` 的结果执行自动细扫。
- 如果没有有效 coarse 结果，则拒绝执行并提示先运行 `dpmzm auto coarse`。

后续可扩展：

```text
dpmzm auto fine p
dpmzm auto fine i
dpmzm auto fine q
dpmzm auto fine all
```

第一版到第二版的关系：

```text
dpmzm auto coarse  ->  找到可用粗初值
dpmzm auto fine    ->  在粗初值附近局部精修
```

---

## 11. 第二版当前落实状态

当前已经在固件中新增 `dpmzm auto fine` 主流程：

- 基于最近一次有效 `dpmzm auto coarse` 结果执行。
- 如果没有有效 coarse 结果，会拒绝执行并提示先运行 `dpmzm auto coarse`。
- 第一阶段执行 `P/I/Q` 宽窗细化：`center +/- 2.0 V`，`step = 0.05 V`。
- 第二阶段执行 `P/I/Q` 小窗口细扫：`step = 0.01 V`。
- 小窗口细扫如果贴边，会先把当前通道偏压设置到贴边 best point，再以该点为中心执行 expanded fine scan。
- 如果 expanded fine scan 仍然贴边，则返回 `NEED_WIDER_WINDOW`。
- 细扫成功后，固件会自动应用最终 `I/Q/P` 偏压。

当前命令关系：

```text
dpmzm auto coarse  ->  板上自动粗捕获
dpmzm auto fine    ->  基于最近一次 coarse 结果做自动细扫
dpmzm auto status  ->  查看最近一次 coarse/fine 状态和结果
```

下一步需要上板验证：

- `auto fine` 每一段曲线是否和手动细扫一致。
- 贴边扩窗规则是否会误触发或漏触发。
- 细扫总耗时是否可以接受。
- 是否需要在 `P -> I -> Q` 后自动再重复一轮小窗口细扫。

---

## 12. 第二版之后再做什么

第二版完成后，再进入第三版：

- 引入小扰动。
- 构造有符号误差。
- 做真正闭环锁定。
- 处理温漂、慢漂和长期稳定性。

工程节奏建议：

1. **V1：自动粗捕获，已完成。**
2. **V2：自动细扫，已经进入固件落实与上板验证阶段。**
3. **V3：小扰动有符号误差与闭环锁定。**

---

## 13. 2026-04-28 第二版自动细扫上板验证

2026-04-28 已经完成一次完整的板上自动流程：

```text
dpmzm auto coarse
dpmzm auto fine
```

本轮自动流程完整跑完，串口解析结果为：

```text
coarse_done = True
fine_done   = True
failed      = False
segments    = 11
```

最终固件自动应用的偏压为：

```text
I = +5.600 V
Q = -6.300 V
P = +1.520 V
```

分阶段结果为：

| 阶段 | 自动选点 | 指标 |
|---|---:|---:|
| `P-QTP wide` | `+1.500 V` | `-71.89 dBm` |
| `P-QTP fine` | `+1.520 V` | `-74.07 dBm` |
| `I-MITP wide` | `+5.650 V` | `-49.19 dBm` |
| `I-MITP fine` | `+5.600 V` | `-69.32 dBm` |
| `Q-MITP wide` | `-6.300 V` | `-50.70 dBm` |
| `Q-MITP fine` | `-6.300 V` | `-74.29 dBm` |

实验观察上，用户通过光功率计确认该最终点大概率正确。这说明第二版自动细扫得到的 RF 谷底点，已经能和实际光功率工作状态相互印证。

本轮没有触发贴边扩窗：

```text
P expanded = no
I expanded = no
Q expanded = no
```

对应完整记录见：

- `docs/scans/2026-04-28-dpmzm-auto-coarse-fine-summary.md`

当前阶段结论：

- **V2 自动细扫主流程已经完成第一轮上板验证。**
- **最终点已经通过光功率计做了定性确认。**
- **下一步建议先做重复性验证，再进入 V3 闭环小扰动误差构造。**
