# Spec 06I - DPMZM 第三版闭环锁定方案

> 状态：V3 implemented / 第一版低速闭环接口已落地，等待长时间稳定性验证  
> 日期：2026-04-28  
> 前置条件：V1 自动粗扫完成，V2 自动细扫完成，并且 `2026-04-28` 自动粗扫 + 自动细扫最终点已通过光功率计做定性确认。  
> 目标：在 `auto coarse + auto fine` 找到可信工作点后，引入小扰动有符号误差，让板子能够低速、稳定地守住最小-最小-正交点。

---

## 1. 一句话目标

第三版不再负责“从全局找点”，而是负责“在第二版已经找到的点附近守点”。

```text
V1: auto coarse  -> 找到可用粗初值
V2: auto fine    -> 找到可信局部最优点
V3: lock         -> 围绕该点低速闭环修正漂移
```

第三版的核心不是再扫大范围，而是构造有符号误差：

```text
往正方向轻轻探一下
往负方向轻轻探一下
比较哪边指标更低
然后把偏压往更低的一边轻微移动
```

---

## 2. 当前阶段共识

基于 2026-04-28 的实验，第二版自动细扫已经能自动应用：

```text
I = +5.600 V
Q = -6.300 V
P = +1.520 V
```

并且该点已经通过光功率计做了定性确认，大概率是正确的最小-最小-正交候选点。

因此第三版不应该再优先解决“找不到点”的问题，而应该优先解决：

- 找到点之后，温漂或慢漂来了怎么办。
- 如何判断某一路偏压应该往正方向修，还是往负方向修。
- 如何避免闭环过冲、误锁或被噪声带跑。
- 如何让闭环过程仍然可观察、可暂停、可人工接管。

---

## 3. 闭环指标选择

第三版沿用第二版已经验证过的三个主指标：

| 通道 | 控制目标 | 主判据 |
|---|---|---|
| `I` | I 路 MITP | `fI = 1000 Hz` 功率最小 |
| `Q` | Q 路 MITP | `fQ = 1200 Hz` 功率最小 |
| `P` | P 路 QTP | `fI+fQ = 2200 Hz` 功率最小 |

暂时不把 `200 Hz = |fI-fQ|` 纳入第三版主闭环判据。

原因：

- 之前实验已经看到 `200 Hz` 受低频干扰明显。
- `P-QTP` 用 `2200 Hz` 做主判据已经能和光功率计结果对上。
- 第三版第一目标是稳定可控，不是一次性融合所有指标。

后续如果 `2200 Hz` 锁定稳定，再考虑把 `200 Hz` 作为辅助健康监测指标，而不是第一版闭环主误差。

---

## 4. 小扰动有符号误差

对某一路偏压 `Vx`，第三版每次只做一个很小的左右探测：

```text
V_plus  = Vx + delta
V_minus = Vx - delta
```

分别测量目标功率：

```text
M_plus  = metric(V_plus)
M_minus = metric(V_minus)
```

判断方向：

```text
如果 M_plus > M_minus:
    负方向更接近谷底，Vx 应该减小

如果 M_plus < M_minus:
    正方向更接近谷底，Vx 应该增大

如果 M_plus ~= M_minus:
    当前已经在谷底附近，保持不动
```

推荐误差形式：

```text
e = (M_plus - M_minus) / (M_plus + M_minus + eps)
```

控制更新：

```text
V_new = V_old - K * e
```

注意：

- 控制内部最好使用线性幅度或线性功率，不要直接用 dBm 做更新量。
- dBm 仍然可以用于串口打印和人眼观察。
- 如果固件内部当前只方便拿到线性 Goertzel 幅值，可以先用幅值做相对比较；后续再统一成线性功率。

---

## 5. 第三版命令接口

第三版建议先实现三个层级的命令。

### 5.1 `dpmzm lock probe`

只探测，不更新偏压。

```text
dpmzm lock probe p
dpmzm lock probe i
dpmzm lock probe q
```

作用：

- 对指定通道执行 `V+delta` 和 `V-delta` 两次测量。
- 打印 `M_plus`、`M_minus`、误差 `e` 和建议方向。
- 测完后恢复到原偏压。
- 用于验证误差符号是否正确。

这一步非常重要。没有确认符号之前，不应该直接进入自动闭环。

### 5.2 `dpmzm lock step`

执行单步闭环更新。

```text
dpmzm lock step p
dpmzm lock step i
dpmzm lock step q
```

作用：

- 对指定通道执行一次左右探测。
- 根据误差计算一个很小的偏压修正量。
- 限幅后应用到当前通道。
- 打印更新前后偏压和指标变化。

这一步用于人工慢慢验证闭环是否真的朝正确方向走。

### 5.3 `dpmzm lock start / stop / status`

进入自动低速闭环。

```text
dpmzm lock start
dpmzm lock stop
dpmzm lock status
```

建议 `lock start` 采用低速循环，不追求快：

```text
P -> I -> P -> Q -> P
```

这样安排的原因：

- `P` 是正交点，受 `I/Q` 变化影响明显。
- `I/Q` 每改一次，`P-QTP` 都可能轻微移动。
- 让 `P` 出现更频繁，可以帮助系统维持正交条件。

---

## 6. 第三版初始参数

第一版闭环参数建议保守一点：

```text
probe_delta_v:
  I = 0.03 V to 0.05 V
  Q = 0.03 V to 0.05 V
  P = 0.03 V to 0.05 V

max_update_step_v:
  0.005 V to 0.010 V

settle_ms:
  5 ms to 20 ms

blocks:
  6 to 10

deadband:
  根据 probe 重复性确定，先用相对误差门限
```

推荐默认值：

```text
delta_v          = 0.05 V
max_step_v       = 0.01 V
gain_k           = 0.01 V
settle_ms        = 10 ms
blocks           = 10
deadband_rel     = 0.03
loop_interval_ms = 200 ms to 1000 ms
```

这些值不是最终参数，只是为了第一轮上板验证足够稳。

---

## 7. 闭环状态机

第三版建议新增独立状态机，不要塞进 `auto coarse/fine`。

建议状态：

```text
IDLE
PROBE_PLUS
PROBE_MINUS
COMPUTE_ERROR
APPLY_STEP
WAIT_NEXT
STOPPED
FAULT
```

建议上下文保存：

```text
enabled
current_axis
current_bias_i/q/p
delta_v
gain_k
max_step_v
deadband_rel
last_metric_plus
last_metric_minus
last_error
last_step_v
update_count
hold_count
fault_count
```

---

## 8. 安全保护

第三版必须有安全边界。闭环控制最怕“看起来在自动工作，实际上被噪声带飞”。

### 8.1 偏压范围限制

所有更新都必须限制在 DAC 安全范围和实验安全范围内：

```text
V_min <= V_new <= V_max
```

如果触边：

- 不继续积分。
- 打印 warning。
- 必要时暂停闭环。

### 8.2 单步限幅

无论误差多大，每次最多只允许更新：

```text
abs(step) <= max_update_step_v
```

### 8.3 死区

如果左右探测差异小于噪声门限：

```text
abs(e) < deadband_rel
```

则不更新偏压，只增加 hold 计数。

### 8.4 指标异常保护

如果测得指标异常，例如：

- ADC 采样失败。
- Goertzel 结果为零或 NaN。
- 指标突变超过合理范围。
- 连续多次无法得到有效误差。

则进入 `FAULT` 或暂停闭环。

### 8.5 人工接管

任意时刻执行：

```text
dpmzm lock stop
```

应立即停止闭环，并保持当前偏压不变。

---

## 9. 推荐实现顺序

第三版不要一次性直接实现完整自动闭环。建议按下面顺序落地：

### 9.1 第一小步：只做 probe

实现：

```text
dpmzm lock probe p
dpmzm lock probe i
dpmzm lock probe q
```

验证目标：

- 左右探测后可以恢复原偏压。
- 串口能打印 `M_plus / M_minus / e / direction`。
- 人工观察确认误差方向正确。

### 9.2 第二小步：单步更新

实现：

```text
dpmzm lock step p
dpmzm lock step i
dpmzm lock step q
```

验证目标：

- 连续执行多次 `lock step p`，`2200 Hz` 能向谷底收敛。
- 连续执行多次 `lock step i`，`1000 Hz` 能向谷底收敛。
- 连续执行多次 `lock step q`，`1200 Hz` 能向谷底收敛。

### 9.3 第三小步：低速自动循环

实现：

```text
dpmzm lock start
dpmzm lock stop
dpmzm lock status
```

初始循环顺序：

```text
P -> I -> P -> Q -> P
```

验证目标：

- 运行 10 到 30 分钟，光功率计读数不明显漂移。
- RF 指标不会系统性恶化。
- 停止闭环后偏压保持在合理点附近。

---

## 10. 与 V2 的衔接关系

推荐第三版入口流程：

```text
dpmzm set pilot-open on
dpmzm auto coarse
dpmzm auto fine
dpmzm lock probe p
dpmzm lock probe i
dpmzm lock probe q
dpmzm lock start
```

如果用户手动设置了可信点，也可以跳过 `auto coarse/fine`：

```text
dpmzm set bias i <value>
dpmzm set bias q <value>
dpmzm set bias p <value>
dpmzm lock probe p
dpmzm lock start
```

但固件应当在 `lock start` 前提示当前是否有有效的 `auto fine` 结果。

---

## 11. 第三版验收标准

第三版第一轮验收不要求“锁得最快”，只要求“方向正确、不会乱跑、能守住”。

验收条件：

1. `lock probe p/i/q` 能稳定给出方向。
2. `lock step p/i/q` 单步更新方向正确。
3. `lock start` 可以连续运行至少 10 分钟。
4. 光功率计读数没有明显变差。
5. `fI`、`fQ`、`fI+fQ` 指标没有系统性恶化。
6. `lock stop` 可以立即停止并保持当前偏压。
7. 出现异常时能进入安全状态，而不是继续更新。

---

## 12. 第三版之后的扩展

第三版先做低速可靠闭环。后续再考虑：

- 多路同时扰动。
- 正交扰动编码，避免三路互相干扰。
- 使用同步检波构造连续误差。
- 根据温漂速度自适应调节 `gain_k`。
- 把光功率计或外部监测量纳入健康检查。
- 参数保存到 Flash，支持掉电恢复。

当前不建议第一轮就做这些，因为我们现在最需要的是一个可解释、可验证、可暂停的闭环雏形。

---

## 13. 当前结论

第三版推荐路线已经明确：

```text
先 probe，确认符号
再 step，确认单步收敛
最后 start，低速自动守点
```

这条路线与当前硬件和固件状态匹配，也能最大程度继承 V1/V2 已经验证过的扫描、测量和导频链路。

---

## 14. 2026-04-29 固件实现状态

第三版第一轮固件已经落地，核心代码文件为：

- `control/inc/ctrl_lock_dpmzm.h`
- `control/src/ctrl_lock_dpmzm.c`
- `app/src/app_main_dpmzm.c`

当前已经实现的命令：

```text
dpmzm lock probe p
dpmzm lock probe i
dpmzm lock probe q
dpmzm lock step p
dpmzm lock step i
dpmzm lock step q
dpmzm lock start
dpmzm lock stop
dpmzm lock status
```

### 14.1 `lock probe`

`probe` 用于只判断方向，不改变最终偏压。

流程为：

1. 记录当前通道偏压 `V0`。
2. 设置到 `V0 + delta`，测一次目标指标。
3. 设置到 `V0 - delta`，测一次目标指标。
4. 恢复到 `V0`。
5. 输出 `metric_plus`、`metric_minus`、相对误差和建议方向。

### 14.2 `lock step`

`step` 在 `probe` 基础上执行一次小步更新。

更新逻辑为：

```text
error = (metric_plus - metric_minus) / (metric_plus + metric_minus + eps)
step  = -gain * error
step  = clamp(step, -max_step, +max_step)
```

若误差落入死区，则保持偏压不动。

### 14.3 `lock start`

`lock start` 启动低速后台循环。当前循环顺序为：

```text
P -> I -> P -> Q -> P
```

这样安排是因为 `P-QTP` 对 `I/Q` 工作点变化更敏感，`P` 需要在 `I` 或 `Q` 更新后更频繁地重新守点。

### 14.4 默认参数

当前固件默认参数：

```text
delta_v          = 0.05 V
gain_v           = 0.01 V
max_step_v       = 0.01 V
deadband_rel     = 0.03
settle_ms        = 10 ms
blocks           = 10
loop_interval_ms = 500 ms
```

这些参数偏保守，目标是先保证不会乱跑。后续如果确认方向和稳定性没有问题，再考虑提高闭环速度。

### 14.5 与自动扫描脚本的衔接

新增脚本：

- `tools/run_dpmzm_positive_branch_flow.py`

该脚本会在扫描和小窗口复现流程结束后自动执行：

```text
dpmzm lock start
dpmzm lock status
```

也就是说，现在完整实验流程已经可以从“自动找点”自然进入“低速守点”。

脚本默认先把三路偏压设置为 `0/0/0 V`，再执行粗扫。这个默认行为来自 2026-04-29 的实测发现：如果继承上一次实验的最终偏压，粗扫可能进入另一条分支，导致后续 `auto fine` 贴边失败。

### 14.6 当前仍需验证

当前 V3 已经完成“接口和基本动作”，但还不能直接宣称长期闭环已经最终完成。后续仍需验证：

1. `lock start` 连续运行 10 到 30 分钟时，光功率计是否保持稳定。
2. `P -> I -> P -> Q -> P` 顺序是否优于其他顺序。
3. `delta/gain/max_step/deadband` 是否需要按通道独立配置。
4. 正 P 分支与负 P 分支的选择规则是否需要固化进板上逻辑。
5. 异常情况下是否能可靠进入 `FAULT` 或被 `lock stop` 人工接管。

阶段性结论：

- **V3 第一版低速闭环固件已经完成。**
- **自动扫描结束后自动进入闭环的实验脚本已经完成。**
- **下一阶段重点是长时间稳定性和参数整定，而不是继续补基础接口。**
