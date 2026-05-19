# Spec 06N - DPMZM 当前闭环控制策略说明

日期: 2026-05-18

状态: 当前固件闭环控制基线文档

用途: 记录现有 `lock start` 闭环算法的真实工作方式、参数、日志含义、已观察到的问题，以及后续改进方向。本文档不描述理想方案，而是尽量忠实整理“现在代码到底怎么控制”。

---

## 1. 当前闭环控制目标

当前 DPMZM 找点完成后，系统希望稳定在:

```text
I-MITP + Q-MITP + P-QTP
```

对应的三个闭环目标频点是:

| 轴 | 工作点目标 | 目标指标 | 固件使用的 metric |
| --- | --- | --- | --- |
| I | I-MITP | I 路一阶导频最小 | `mag_fi`, 1000 Hz |
| Q | Q-MITP | Q 路一阶导频最小 | `mag_fq`, 1200 Hz |
| P | P-QTP | I/Q 和频交调最小 | `mag_fsum`, 2200 Hz |

当前闭环不使用 `200 Hz = |fI - fQ|` 作为控制目标。原因是实验中 200 Hz 受低频噪声、漂移、工频附近扰动影响较明显，作为主控制量不够可靠。

---

## 2. 相关代码位置

当前闭环控制主要由两个文件实现:

| 文件 | 作用 |
| --- | --- |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_lock_dpmzm.c` | 闭环算法核心: 三点探测、误差计算、步长计算、锚点窗口限制 |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\src\app_main_dpmzm.c` | 串口命令、默认参数、周期调度、把算法结果真正写入 DAC |

当前用户侧常用命令:

```text
dpmzm auto coarse
dpmzm auto fine
dpmzm lock start
dpmzm lock status
dpmzm lock stop
dpmzm lock probe i|q|p
dpmzm lock step i|q|p
```

其中:

- `auto coarse` 和 `auto fine` 负责找点。
- `lock start` 只负责从当前偏压点启动闭环。
- `lock start` 启动时会把当前 I/Q/P 偏压记录为闭环锚点 anchor。
- `lock status` 只打印状态，不主动执行控制。
- 周期性控制由主循环中的 lock task 自动执行。

---

## 3. 闭环启动流程

执行:

```text
dpmzm lock start
```

固件做的事情是:

1. 尝试打开 onboard pilot。
2. 读取当前三路偏压目标值:

```text
I = 当前 I bias target
Q = 当前 Q bias target
P = 当前 P bias target
```

3. 将当前三路偏压保存为 anchor:

```text
anchor_I = 当前 I
anchor_Q = 当前 Q
anchor_P = 当前 P
```

4. 使能闭环状态机。

日志一般类似:

```text
[dpmzm][lock] start: sequence P-I-Q gradient, anchor I=+3.680V Q=+8.190V P=+2.200V
```

这里的 anchor 很重要。当前闭环不是无限范围追踪，而是在 anchor 附近一个小窗口内微调。

---

## 4. 当前控制顺序

当前闭环按固定顺序轮询三路:

```text
P -> I -> Q -> P -> I -> Q -> ...
```

也就是说，每一次闭环周期只调整一个轴。这样做的好处是避免三路同时变化导致因果关系完全混在一起；缺点是 DPMZM 三路实际强耦合，单轴局部最优不一定等于三轴整体最优。

当前状态机每次执行时会打印:

```text
[dpmzm][lock] cycle axis=p
[dpmzm][lock] cycle axis=i
[dpmzm][lock] cycle axis=q
```

---

## 5. 每个轴的一次闭环探测

以 Q 轴为例。当前 Q 偏压为 `center`，探测步长为 `delta=0.01 V`，则固件会测三次:

```text
center = 当前 Q
plus   = 当前 Q + 0.01 V
minus  = 当前 Q - 0.01 V
```

每个点都会临时设置偏压，然后执行一次小块采集，得到对应目标频点 metric。

Q 轴对应的日志可能是:

```text
[dpmzm][lock] probe axis=q valid=yes error=OK
  center: +8.165V plus=+8.175V minus=+8.155V
  metric0: 0.001371758 (-47.25 dBm) dc=-0.002180V best=yes
  metric+: 0.001547179 (-46.21 dBm) dc=-0.002213V
  metric-: 0.001211400 (-48.33 dBm) dc=-0.002191V
```

其中:

| 字段 | 含义 |
| --- | --- |
| `metric0` | 当前中心点的目标频点幅值 |
| `metric+` | 当前轴加 `+0.01 V` 后的目标频点幅值 |
| `metric-` | 当前轴减 `-0.01 V` 后的目标频点幅值 |
| `dBm` | 将 metric 按 50 欧姆等效功率换算后的显示值 |
| `dc` | 同一采样段的 ADC 直流量，仅打印，当前闭环不参与控制 |
| `best=yes` | 当前中心点是否优于两侧点 |

注意: 这里的 dBm 不是光谱仪上的光功率，也不是光载波功率，而是 ADC 电信号中特定频率分量的等效功率。

---

## 6. metric 与目标函数

当前固件对三个轴使用的 metric 是:

```text
I axis: mag_fi    -> 1000 Hz
Q axis: mag_fq    -> 1200 Hz
P axis: mag_fsum  -> 2200 Hz
```

目标函数为:

```text
objective = metric^2
```

也就是说，当前闭环的目标是让对应频点幅值的平方最小。DC 不参与目标函数。

这一点非常关键:

- I 闭环只关心 1000 Hz。
- Q 闭环只关心 1200 Hz。
- P 闭环只关心 2200 Hz。
- 当前没有把光谱仪载波功率、PD DC、对方轴劣化、噪声底等信息放进目标函数。

---

## 7. 误差计算逻辑

三点测完后，固件先判断中心点是不是已经最好:

```text
if objective(center) <= objective(plus)
and objective(center) <= objective(minus):
    hold-center
```

这一步的意思是，如果当前点已经比左右两侧都低，就认为当前点在谷底附近，不继续推偏压。

如果中心点不是最好，则计算归一化左右差:

```text
error = (objective(plus) - objective(minus))
        / (objective(plus) + objective(minus) + eps)
```

这个 error 的符号含义:

| 情况 | 物理含义 | 控制方向 |
| --- | --- | --- |
| `error > 0` | `plus` 比 `minus` 更差，说明负方向更好 | 往负方向走 |
| `error < 0` | `minus` 比 `plus` 更差，说明正方向更好 | 往正方向走 |
| `abs(error) < deadband` | 左右差异不明显 | hold |

如果误差小于死区:

```text
abs(error) < 0.03
```

则本次不移动。

---

## 8. 步长计算逻辑

如果没有进入 hold，则当前固件按比例生成步长:

```text
raw_step = -gain * error
```

当前默认:

```text
gain = 0.003 V
max_step = 0.003 V
```

因此单次最大更新步长约为:

```text
±3 mV
```

再经过锚点窗口和全局偏压范围限制:

```text
new_bias = clamp(center + applied_step, min_limit, max_limit)
```

日志一般类似:

```text
[dpmzm][lock] step axis=q requested=-0.000720V applied=-0.000720V new=+8.164238V hold=no clamp=no
```

含义:

| 字段 | 含义 |
| --- | --- |
| `requested` | 算法希望走的步长 |
| `applied` | 限幅后真正应用的步长 |
| `new` | 应用后的新偏压 |
| `hold=yes` | 本次进入死区或中心最好，没有移动 |
| `clamp=yes` | 被 anchor window 或全局范围截断 |

---

## 9. 默认参数表

当前固件默认闭环参数如下:

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `delta_v` | `0.01 V` | 三点探测时 `center ± delta` 的探测距离 |
| `gain_v` | `0.003 V` | error 到偏压步长的比例 |
| `max_step_v` | `0.003 V` | 单次最大移动步长 |
| `deadband_rel` | `0.03` | error 死区，小于该值则不移动 |
| `settle_ms` | `10 ms` | 每次设置偏压后的等待时间 |
| `iq_blocks` | `4` | I/Q 轴每个探测点采集块数 |
| `p_blocks` | `10` | P 轴每个探测点采集块数 |
| `iq_anchor_window_v` | `±0.20 V` | I/Q 相对 anchor 的允许移动窗口 |
| `p_anchor_window_v` | `±0.30 V` | P 相对 anchor 的允许移动窗口 |
| `loop_interval_ms` | `500 ms` | 闭环任务触发间隔 |
| `min_bias_v / max_bias_v` | `-10 V / +10 V` | 全局偏压限制 |

这些参数体现了当前策略的定位:

- 不做大范围重新找点。
- 只在 auto fine 找到的点附近做小幅修正。
- 尽量避免闭环把偏压拖得太远。

---

## 10. 一次完整自动闭环的时间结构

一次轴更新需要三次测量:

```text
center -> plus -> minus
```

每个测量点都需要:

1. 设置偏压。
2. 等待 settle。
3. ADC 采集 blocks。
4. 提取目标频点 metric。

因此:

- I 轴一次更新使用 3 个点，每点 4 blocks。
- Q 轴一次更新使用 3 个点，每点 4 blocks。
- P 轴一次更新使用 3 个点，每点 10 blocks。

P 轴更慢，但 P 的 2200 Hz 交调项本身更弱，所以目前保留较多 blocks。

---

## 11. 当前策略的优点

当前闭环策略有几个明显优点:

1. 实现简单，容易读日志定位问题。
2. 每次只动一个轴，不会同时改变 I/Q/P。
3. 有 center-best 判断，避免在谷底附近被左右不对称持续推走。
4. 有 deadband，避免在左右差异很小时抖动。
5. 有 anchor window，避免偏压离开 auto fine 找到的局部工作区太远。
6. P 轴使用更多 blocks，有助于降低 2200 Hz 弱交调项测量抖动。

这些设计让当前闭环作为“第一版可运行控制器”是成立的。

---

## 12. 当前策略暴露的问题

### 12.1 三路强耦合没有被显式处理

DPMZM 的 I/Q/P 不是独立系统。当前闭环每次只看当前轴的单个目标频点:

```text
调 I 时只看 1000 Hz
调 Q 时只看 1200 Hz
调 P 时只看 2200 Hz
```

但实际情况可能是:

```text
I 轴往某个方向移动后，I 指标略微变好，
但 Q 轴原来的 MITP 条件被破坏，Q 指标大幅变坏。
```

近期日志中已经出现类似现象:

```text
auto fine 后 Q 点很好，接近 -70 dBm 级别；
lock 运行一段时间后 Q 指标变成 -47 dBm / -39 dBm 级别；
Q 自身偏压只漂了几十 mV，但 I 轴漂了约百 mV 量级。
```

这说明问题不一定是 Q 自己走太远，而可能是 I/Q/P 耦合导致原来的 Q 谷底被移动或破坏。

### 12.2 当前目标函数没有全局守护

当前单轴更新只判断本轴 metric 是否下降，没有检查:

- I 更新后 Q 是否变坏。
- Q 更新后 I 是否变坏。
- P 更新后 I/Q 是否变坏。
- 三路综合目标是否变坏。
- 光谱仪载波是否变坏。

因此局部单轴改善可能造成全局工作点变差。

### 12.3 DC 当前只打印，不参与控制

日志中每次都会打印 `dc`，例如:

```text
metric0: ... dc=-0.002180V
```

但当前闭环并没有用 DC 判断 MITP 是否可信，也没有用 DC 做失锁判断。

这意味着如果目标频点 metric 在噪声底附近，算法仍可能对噪声做梯度下降。

### 12.4 缺少信号存在性判断

如果 PD 断开、光路断开、RF 状态异常、ADC 只采到噪声，当前算法仍可能得到一个看似较低的 metric，并认为控制效果很好。

这就是我们观察到的风险:

```text
即使断掉 PD，日志中的控制指标也可能看起来正常。
```

这说明当前闭环缺少:

- PD DC 下限判断。
- ADC RMS 判断。
- 目标频点 SNR 判断。
- 旁瓣/噪声底对比判断。
- `NO_SIGNAL` 或 `LOW_CONFIDENCE` 状态。

### 12.5 Q 路窄谷底容易被拖出

有些调制器或某些偏压区域，Q-MITP 谷底非常窄。即使 Q 自己只移动几十 mV，或者 I/P 轻微变化，也可能导致 Q 的 1200 Hz 指标从深谷变成浅谷。

这类情况下，当前 `±0.20 V` anchor window 虽然限制了最大偏移，但不能保证一直停留在真实谷底。

### 12.6 anchor window 只是边界，不是回退机制

当前 anchor window 只做:

```text
不允许偏压离 anchor 超过窗口
```

它不会做:

```text
如果指标明显比 anchor 变差，则回退到 anchor
```

所以它是“防走飞”，不是“质量守护”。

---

## 13. 日志解读示例

以 Q 轴为例:

```text
[dpmzm][lock] cycle axis=q
[dpmzm][lock] probe axis=q valid=yes error=OK
  center: +8.165V plus=+8.175V minus=+8.155V
  metric0: 0.001371758 (-47.25 dBm) dc=-0.002180V best=yes
  metric+: 0.001547179 (-46.21 dBm) dc=-0.002213V
  metric-: 0.001211400 (-48.33 dBm) dc=-0.002191V
  e: -0.092000 direction=positive
[dpmzm][lock] step axis=q requested=+0.000276V applied=+0.000276V new=+8.165276V hold=no clamp=no
```

可以这样读:

1. 当前正在更新 Q。
2. 当前中心点是 `+8.165V`。
3. 固件比较 `+8.175V` 和 `+8.155V` 两边的 1200 Hz 指标。
4. 目标是让 1200 Hz 越小越好。
5. 如果 `metric-` 更小，说明负方向更好；如果 `metric+` 更小，说明正方向更好。
6. 算法把左右差换成 error。
7. 按 `-gain * error` 得到一个 mV 级步长。
8. 写入新的 Q 偏压。

如果看到:

```text
hold=yes
```

说明本次没有移动。

如果看到:

```text
clamp=yes
```

说明算法想移动，但被 anchor window 或全局偏压范围限制住了。

---

## 14. 当前策略与找点流程的关系

当前完整流程是:

```text
auto coarse -> auto fine -> lock start
```

`auto fine` 的输出决定闭环 anchor:

```text
I_anchor = auto fine 得到的 I
Q_anchor = auto fine 得到的 Q
P_anchor = auto fine 得到的 P
```

闭环并不是重新找全局最优点，而是在这个 anchor 附近局部微调。

因此:

- 如果 auto fine 找点正确，闭环应该只做微小补偿。
- 如果 auto fine 找点错误，闭环大概率救不回来。
- 如果 auto fine 找点正确但闭环跑坏，说明闭环策略本身或信号可信度判断存在问题。

近期的关键发现是:

```text
有些情况下 auto fine 找到的 Q 点很好，
但是 lock 运行后 Q 指标明显劣化。
```

所以当前重点已经从“找点是否正确”转移到“闭环是否会破坏已找到的点”。

---

## 15. 为什么当前算法可能看起来在控制，但光谱仍漂移

当前算法控制的是 ADC 中的三个电频率指标:

```text
1000 Hz, 1200 Hz, 2200 Hz
```

但我们最终关心的光谱现象包括:

- 光载波抑制。
- 两个边带平衡。
- 载波长期漂移。
- RF 加载状态下的真实调制输出。

如果 ADC 频点指标与光谱载波之间不是严格一一对应，或者当前频点已经接近噪声底，那么闭环可能出现:

```text
电指标看起来还可以，
但光谱仪上载波仍慢慢漂。
```

这不是矛盾，而是说明当前控制目标还不够完整。

---

## 16. 当前不完善点总结

当前闭环主要缺少以下能力:

1. 信号存在性判断。
2. metric 置信度判断。
3. 目标频点相对噪声底的 SNR 判断。
4. I/Q/P 耦合后的全局目标判断。
5. 更新后复查机制。
6. 指标变差后的回退机制。
7. anchor 点质量保持机制。
8. 对光谱仪观测目标的间接约束。
9. 对 RF on/off 状态差异的显式处理。
10. 对 PD 断开、光路异常、ADC 只剩噪声的保护。

---

## 17. 后续修改建议

### 17.1 加信号可信度门限

每次闭环测量前或测量后，应判断当前数据是否可信:

```text
if PD_DC < dc_min:
    lock state = NO_SIGNAL
    hold all axes

if target_metric 接近 noise floor:
    lock state = LOW_CONFIDENCE
    hold or reduce gain
```

这一步可以避免“断 PD 仍然看起来控制很好”的假象。

### 17.2 加 anchor 质量守护

在 `lock start` 时保存 anchor 附近的基准指标:

```text
I_anchor_metric
Q_anchor_metric
P_anchor_metric
```

闭环运行中如果某一轴指标明显劣化:

```text
metric_current > metric_anchor * threshold
```

则不继续梯度下降，而是:

1. 回退到 anchor。
2. 或执行该轴小窗口复查。
3. 或进入 `RECHECK` 状态。

### 17.3 I/Q 更新后复查对方轴

由于 I/Q 强耦合，可以加入:

```text
更新 I 后，快速测一次 Q 指标。
如果 Q 明显变坏，则撤销 I 更新。

更新 Q 后，快速测一次 I 指标。
如果 I 明显变坏，则撤销 Q 更新。
```

这样可以避免单轴局部改善破坏另一轴 MITP。

### 17.4 P 更新后复查 I/Q 或综合指标

P 调整会影响 I/Q 合成后的交调，也可能影响 I/Q 指标。可以考虑:

```text
P step 后:
    检查 2200 Hz 是否下降
    同时检查 1000/1200 Hz 是否没有大幅恶化
```

如果 P 改善 2200 Hz 很少，却显著破坏 I/Q，则应撤销。

### 17.5 从单目标改成小型综合目标

未来可以把目标函数从单一 metric 改成:

```text
J = wI * mag_fi^2 + wQ * mag_fq^2 + wP * mag_fsum^2 + wDC * dc_penalty
```

其中:

- 调 I 时仍以 `mag_fi` 为主，但加入 Q/P 弱惩罚。
- 调 Q 时仍以 `mag_fq` 为主，但加入 I/P 弱惩罚。
- 调 P 时仍以 `mag_fsum` 为主，但加入 I/Q 弱惩罚。

这会比当前单轴单指标更稳，但实现和调参也更复杂。

### 17.6 加锁定状态分级

建议未来状态机区分:

```text
LOCKED
TRACKING
HOLD_WEAK
LOW_CONFIDENCE
NO_SIGNAL
RECHECK
FAULT
```

这样日志能直接反映当前控制是否可信，而不是只输出 metric。

---

## 18. 当前结论

当前闭环算法可以概括为:

```text
基于 auto fine 结果作为 anchor 的 P-I-Q 单轴轮询三点梯度下降。
```

它现在能做:

- 在 anchor 附近小步调整 I/Q/P。
- 让当前轴目标频点局部下降。
- 防止偏压无限漂远。
- 输出比较完整的三点探测日志。

但它还不能保证:

- 三路综合最优。
- I/Q/P 互不破坏。
- 光谱载波一定被稳定压住。
- PD 断开或信号异常时能识别失败。
- 指标变差后自动回退到找点结果。

因此下一阶段优化重点不应该只调 `gain/delta/blocks`，而应该在当前三点法外面增加:

```text
信号可信度判断 + anchor 质量守护 + 更新后复查/撤销机制
```

这会比单纯继续减小步长或增大 blocks 更有价值。

---

## 19. 2026-05-19 PC 侧自适应步长实验方案

### 19.1 修改动机

2026-05-19 的实验中，我们用同一套固定 `gain/max_step` 控制不同调制器，观察到一个关键现象:

```text
同一个闭环算法，对不同调制器的跟踪能力不同。
```

其中一组数据里，Q 路在固件闭环下连续 94 次都判断负方向更优:

```text
Q direction = negative: 94 / 94
Q bias drift: 约 -55 mV
Q metric: 从约 -58 dBm 劣化到约 -39 dBm
```

这说明固定步长策略可能存在两个问题:

1. 如果某个调制器的 Q 路谷底漂移较快，`3 mV` 级最大步长可能追不上。
2. 如果把所有调制器都固定改成更大步长，又可能在稳定调制器上过冲。

因此新的实验策略不是简单把 Q 步长永久调大，而是先在 PC 侧脚本中加入“每轴自适应步长”，验证它能否对不同调制器自动调整追踪速度。

### 19.2 自适应步长的基本思想

探测步长和控制步长分离:

```text
探测步长 delta = 0.01 V
控制步长 max_step = 自适应变化
```

`delta` 仍然保持较小，用来判断局部梯度，避免探测本身严重扰动工作点。

`gain/max_step` 则根据控制过程动态缩放:

```text
如果某轴连续多次朝同一方向移动:
    说明谷底可能在持续漂移，或者当前点离谷底还较远
    -> 增大该轴 gain/max_step

如果某轴方向反复翻转:
    说明已经接近谷底，或者步长过大
    -> 减小该轴 gain/max_step

如果中心点已经最好，或者误差进入 deadband:
    -> hold，并逐步把步长恢复到较小值
```

这使算法具备一定自适应能力:

- 漂移慢的调制器使用小步长，避免抖动。
- 漂移快的调制器自动加大步长，提高追踪能力。
- 不需要提前知道调制器规格。

### 19.3 当前脚本实现

本次先只修改 PC 侧脚本，不改固件。

涉及文件:

| 文件 | 修改内容 |
| --- | --- |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_gradient_lock_test.py` | 增加每轴自适应状态机 |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_gradient_lock_test_com8.bat` | 默认开启自适应步长 |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_auto_then_gradient_com8.bat` | 找点后不开固件闭环，改用 PC 自适应闭环 |

新增或保留的关键参数:

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--delta` | `0.01 V` | 三点探测半步长 |
| `--gain` | `0.003 V` | 基础比例增益 |
| `--max-step` | `0.003 V` | 基础最大控制步长 |
| `--adaptive-step` | bat 默认开启 | 启用自适应缩放 |
| `--adaptive-hard-max-step` | `0.010 V` | 自适应后的单次硬上限 |
| `--adaptive-growth` | `1.5` | 连续同方向后的放大倍率 |
| `--adaptive-shrink` | `0.5` | 翻转或 hold 后的缩小倍率 |
| `--adaptive-same-direction` | `3` | 连续同方向几次后开始放大 |
| `--adaptive-min-scale` | `0.5` | 最小缩放倍率 |
| `--adaptive-max-scale` | `4.0` | 最大缩放倍率 |

当前 bat 默认仍使用:

```text
sequence = P -> I -> Q -> ...
cycles   = 100
blocks   = I/Q 4, P 10
```

但不再默认让 Q 一开始就固定使用更大步长，而是:

```text
I/Q/P 初始 gain/max_step 相同
如果 Q 确实需要追踪更快，它会在连续同方向判断后自动放大
```

### 19.4 日志与 CSV 新增字段

PC 侧梯度日志现在会显示当前有效增益、有效最大步长和缩放倍率:

```text
gain=0.0045V max=0.0045V scale=1.50->2.25 step=-0.00450V
```

含义:

| 字段 | 含义 |
| --- | --- |
| `gain` | 当前轴本次实际使用的有效 gain |
| `max` | 当前轴本次实际使用的有效 max_step |
| `scale=a->b` | 本次测量前后自适应倍率变化 |
| `step` | 本次最终写入偏压的实际步长 |

CSV 中新增:

```text
adaptive_scale
effective_gain_v
effective_max_step_v
```

这样后续可以判断:

- 哪一路触发了加速。
- 触发加速后指标是否变好。
- 是否出现方向反复翻转导致缩步。

### 19.5 使用方式

如果希望完整执行“固件找点 + PC 自适应闭环”，使用:

```bat
C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_auto_then_gradient_com8.bat
```

流程是:

```text
firmware auto coarse
-> firmware auto fine
-> 不启动 firmware lock
-> 以 auto fine 结果作为 anchor
-> PC 侧执行 P -> I -> Q 自适应梯度闭环
-> 自动保存 serial log / progress log / CSV / PNG
```

如果只想从当前偏压点启动 PC 自适应闭环，使用:

```bat
C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_gradient_lock_test_com8.bat
```

如果要复现旧的固定步长行为，可以在 Python 脚本中加:

```text
--no-adaptive-step
```

### 19.6 下一步判断标准

下一轮实验重点看三件事:

1. Q 路是否仍然长期单方向移动。
2. Q 路加速后，`mag_fq` 是否能维持在原找点附近，而不是持续劣化。
3. I/P 是否因为 Q 加速而被破坏。

如果结果是:

```text
Q 自适应加速后，Q 指标明显更稳，同时 I/P 没有恶化
```

则说明“不同调制器需要不同追踪速度”的判断基本成立，可以考虑把这套自适应步长移植到固件。

如果结果是:

```text
Q 仍然持续劣化，或者 I/P 被明显破坏
```

则说明问题不只是步长不够，还需要引入:

- 更新后复查。
- 指标变差回退。
- I/Q/P 综合目标函数。
- 信号可信度与噪声底判断。

---

## 20. 2026-05-19 PD DC 上升守护与触发式小窗复查

### 20.1 修改动机

自适应梯度闭环解决的是“某一路是否追得上谷底漂移”的问题，但实验中又观察到:

```text
ADC 频点指标看起来不错，
光谱仪上的载波仍然会长时间缓慢升高。
```

这说明当前闭环目标函数仍然缺少一个对“平均光功率/载波泄漏趋势”的守护量。

PD 输出的 ADC 直流分量可以直观反映平均光功率变化。因此我们引入:

```text
pd_dc_anchor = 找点完成或闭环刚启动时的 PD DC 参考
pd_dc_now    = 闭环运行中每次采样得到的 PD DC
```

注意: 这里的 DC 是 ADC 采集到的 PD 直流分量，不是 I/Q/P DAC 偏压直流。

### 20.2 只对 PD DC 上升触发

MITP/QTP 调整过程中，某些正确操作本身可能会降低平均光功率。因此不能把 DC 变小当成异常。

当前规则是:

```text
PD DC 下降:
    不触发，认为不是危险信号

PD DC 上升:
    计算相对 anchor 的上升 dB
    对最近 N 次 rise_db 做滑动平均
    平均值超过上升阈值并连续出现多次后，触发小窗复查
    只有平均值回落到释放阈值以下，才清除告警状态
```

计算方式:

```text
dc_ref = max(abs(pd_dc_anchor), dc_floor)
pd_dc_rise_db = 20 * log10(abs(pd_dc_now) / dc_ref)
```

为避免正常 DC 呼吸式波动误触发，实际触发使用滑动平均和滞回:

```text
pd_dc_rise_avg_db = mean(last N samples of pd_dc_rise_db)

触发阈值:
    pd_dc_rise_avg_db > 4 dB

释放阈值:
    pd_dc_rise_avg_db < 2 dB
```

也就是说，在 `2 dB ~ 4 dB` 之间不会来回跳变，只有明确超过 `4 dB` 才进入告警，明确回落到 `2 dB` 以下才释放。

当前 PC 脚本默认:

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--dc-guard` | 开启 | 启用 PD DC 上升守护 |
| `--dc-guard-threshold-db` | `4.0 dB` | 平均 PD DC 上升超过该阈值后进入告警 |
| `--dc-guard-release-db` | `2.0 dB` | 平均 PD DC 回落到该阈值以下才释放 |
| `--dc-guard-avg-window` | `5` | 对最近 5 次 `pd_dc_rise_db` 做平均 |
| `--dc-guard-trigger-count` | `3` | 平均值连续 3 次超过阈值才触发 |
| `--dc-guard-floor` | `1e-5 V` | 防止 anchor DC 过小导致比例失真 |
| `--dc-guard-cooldown-steps` | `12` | 触发一次复查后，等待若干步再允许下一次触发 |

这样做的目的:

- 避免单次噪声尖峰误触发。
- 避免 DC 在阈值附近上下波动时反复误触发。
- 避免 DC 变小这种可能正确的操作被误判。
- 避免频繁小窗复查打扰正常闭环。

### 20.3 触发后的局部复查

触发 PD DC 守护后，脚本暂停普通梯度更新，执行一次局部小窗复查。

当前默认复查顺序:

```text
I -> Q -> P
```

复查窗口:

| 轴 | 窗口 | 步进 | blocks |
| --- | --- | --- | --- |
| I | 当前 I 附近 `±0.08 V` | `0.01 V` | `4` |
| Q | 当前 Q 附近 `±0.08 V` | `0.01 V` | `4` |
| P | 当前 P 附近 `±0.10 V` | `0.01 V` | `10` |

每个轴复查时，先扫描局部窗口，再优先从满足 DC 限制的点中选择目标 metric 最小的点:

```text
候选点要求:
    abs(pd_dc_candidate) <= dc_limit

dc_limit = abs(pd_dc_anchor) * 10^(threshold_db / 20)
```

如果没有任何点满足 DC 限制，则退化为在所有局部点中选择对应 metric 最小的点，并在日志中保留 `valid_dc=0` 方便后续判断。

### 20.4 anchor 更新策略

为了避免系统慢慢把一个已经变差的 DC 状态当成新的正常状态，当前策略比较保守:

```text
只有复查后找到的 best_dc 不高于原 anchor 时，
才允许更新 pd_dc_anchor。
```

也就是说:

- 如果复查把 PD DC 拉回更低水平，可以更新 anchor。
- 如果复查后 PD DC 仍然高于原 anchor，则保留原 anchor，后续仍能继续发现 DC 上升风险。

### 20.5 日志与 CSV 字段

梯度日志现在会额外打印:

```text
dc=+0.002180V rise=+4.35dB guard=rise/2
```

含义:

| 字段 | 含义 |
| --- | --- |
| `dc` | 当前三点探测 center 点对应的 PD DC |
| `rise` | 相对 `pd_dc_anchor` 的 DC 上升 dB |
| `avg` | 最近 N 次 `rise` 的滑动平均 |
| `guard=ok` | 未超过阈值 |
| `guard=armed` | 已超过释放阈值但还未满足触发条件 |
| `guard=rise/n` | 滑动平均超过上升阈值，当前连续计数为 n |
| `guard=trigger` | 达到触发次数，开始小窗复查 |
| `guard=cooldown` | 刚复查过，暂时不再触发 |

CSV 新增字段:

```text
pd_dc_center
pd_dc_anchor
pd_dc_rise_db
pd_dc_rise_avg_db
pd_dc_trigger_count
dc_guard_event
```

这样后续可以把光谱仪载波变化和 `pd_dc_rise_db` 对齐分析，判断 PD DC 是否确实能作为载波恶化的代理指标。

### 20.6 当前实现位置

本功能目前只在 PC 侧脚本中实现，不修改固件。

涉及文件:

| 文件 | 内容 |
| --- | --- |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_gradient_lock_test.py` | 实现 PD DC 守护、触发计数、小窗复查、CSV/图像字段 |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_auto_then_gradient_com8.bat` | 找点后默认启用 PC 自适应闭环 + PD DC 守护 |
| `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_gradient_lock_test_com8.bat` | 从当前点启动 PC 自适应闭环 + PD DC 守护 |

完整实验推荐继续使用:

```bat
C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\tools\run_dpmzm_auto_then_gradient_com8.bat
```

流程:

```text
firmware auto coarse
-> firmware auto fine
-> 不开启 firmware lock
-> PC 侧 P-I-Q 自适应梯度闭环
-> PD DC 上升超过阈值时触发 I/Q/P 小窗复查
-> 自动保存 serial log / progress log / CSV / PNG
```

### 20.7 后续验证重点

下一步实验需要重点观察:

1. `pd_dc_rise_db` 是否和光谱仪载波升高同步。
2. DC 守护触发后，小窗复查是否能把载波重新压下去。
3. DC 守护是否误触发太频繁。
4. 小窗复查是否会破坏原来已经很好的 I/Q/P 频点指标。

如果验证成立，后续可以考虑把这套逻辑移植进固件，形成:

```text
自适应步长 + PD DC 上升守护 + 触发式小窗复查
```

这是比“固定周期复查”更温和的方案，因为系统稳定时不会主动打扰工作点，只有平均光功率明显上升时才介入。

## 21. 2026-05-19 阶段性验证结论

### 21.1 最新验证数据

本轮重点查看了下面这组 PC 侧梯度闭环日志:

```text
C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data\2026-05-19_114525_dpmzm_gradient_lock_progress.log
C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data\2026-05-19_114525_dpmzm_gradient_lock_serial.log
```

对应图像:

```text
C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient\2026-05-19_114525_dpmzm_gradient_lock_steps.png
```

统计结果:

| 项目 | 数值 | 说明 |
| --- | ---: | --- |
| 梯度更新步数 | 336 | 每一步只处理 I/Q/P 中的一个轴 |
| 串口扫描命令 | 1010 | 基本对应每步 `minus / center / plus` 三点探测 |
| `guard=ok` | 336 | 每一步 PD DC 守护都处于正常状态 |
| `DC guard trigger` | 0 | 本轮没有触发 DC 守护小窗复查 |
| `hold-center-best` | 223 | 多数情况下中心点本来就是三点中最优点 |
| 实际梯度移动 | 约 97 | 只有旁边点更好时才小步移动 |

### 21.2 重要澄清: “小窗扫描”不等于 “DC guard 复查”

这次实验中看到的频繁小窗扫描，实际是 PC 侧梯度锁定算法的正常三点探测:

```text
center - delta
center
center + delta
```

当前默认:

```text
delta = 0.01 V
axis sequence = P -> I -> Q -> ...
```

也就是说，每一次闭环更新都会围绕当前轴做一个很小的局部探测，然后判断:

```text
如果 center 最好 -> 不动
如果 plus 更好   -> 朝 plus 方向小步移动
如果 minus 更好  -> 朝 minus 方向小步移动
```

这和 PD DC 守护触发的小窗复查不是同一个机制。

PD DC 守护复查只有在平均 PD DC 相对 anchor 上升超过阈值，并且连续满足触发条件时才会执行。本轮日志中没有出现该触发。

### 21.3 为什么这次效果明显更好

从日志看，效果改善主要来自三点小窗梯度法本身:

1. 每次只动一个轴，避免 I/Q/P 同时变化导致归因混乱。
2. 每次移动前都实际测量 `minus / center / plus`，不会盲目沿一个方向跑。
3. 多数情况下 `center` 是最优点，算法会保持不动，避免过度控制。
4. 当旁边点确实更好时，算法只施加毫伏级小步长，扰动较小。
5. 自适应步长为不同调制器提供了追踪能力，但仍受硬上限约束。

因此当前更准确的理解是:

```text
三点小窗梯度探测 = 当前闭环控制主体
PD DC guard       = 载波/平均光功率恶化时的保护与重定位机制
```

### 21.4 当前仍需注意的问题

本轮日志末尾出现一次 P 轴 center 点扫描长时间未返回:

```text
grad 337 p center still running, elapsed 20s
grad 337 p center still running, elapsed 40s
```

这说明控制算法本身有效，但串口扫描/固件返回路径仍存在偶发等待风险。

后续需要单独排查:

1. 固件是否偶发没有输出 `DPMZMSUM`。
2. 脚本是否需要扫描超时后的自动重试。
3. P 路 `blocks=10` 时是否更容易出现等待过长。
4. 串口接收缓冲区是否偶发漏读或被上一条命令残留干扰。

### 21.5 下一步建议

短期建议保留当前 PC 侧三点小窗梯度法，因为它已经表现出稳定控制能力。

下一步优先级:

1. 给单点扫描增加超时重试，避免一次 `center` 卡住导致整轮控制停止。
2. 继续记录光谱仪载波功率，并和 `pd_dc_rise_avg_db` 对齐，验证 PD DC 是否能可靠代理载波恶化。
3. 如果后续确实观察到 DC guard 频繁触发且有效，再考虑把“触发式小窗复查”升级为固件内的保护状态机。
4. 暂时不要把 DC guard 误认为主控制器；目前主控制器仍是三点小窗梯度法。
