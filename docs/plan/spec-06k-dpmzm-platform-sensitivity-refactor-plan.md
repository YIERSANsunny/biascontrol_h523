# Spec 06K - DPMZM 平台灵敏度与隐式解问题阶段总结

日期: 2026-05-08

## 1. 当前阶段结论

当前 DPMZM 自动找点和闭环控制链路已经具备完整实验能力:

- 固件端支持 `dpmzm auto coarse`, `dpmzm auto fine`, 手动 `dpmzm scan ...`, `dpmzm lock start/stop/status/probe`。
- 脚本端支持一键执行完整正分支流程, 保存串口日志, 保存 metrics CSV, 并自动绘制所有扫描阶段总览图。
- 导频, ADC 采样率, Goertzel 频点和电脑端 FFT 已完成多轮校准, 当前主要矛盾不再是采样率或频点错配。
- 闭环控制已经改为以 P 路为主, I/Q 通过扫描末尾小窗口复查做锚点保持, 避免 I/Q 连续闭环造成慢漂。
- `lock probe` 已加入中心点保护和 `hold-weak` 逻辑, 避免在谷底附近因为左右不对称而被差分控制推走。

当前最大的矛盾已经从“能不能自动找点”转为“自动找点时如何正确解释隐式解, 平台点和 P 路灵敏度之间的关系”。

## 2. 当前已完成的关键改动

### 2.1 粗扫步进回到 0.5 V

为了缩短粗扫时间并复现此前较稳定的实验流程, 固件 `handle_auto_coarse()` 中默认粗扫参数为:

```text
range = -9.0 V .. +9.0 V
step  = 0.5 V
IQ blocks = 4
P blocks  = 10
```

精细定位仍由后续宽窗, 转折点搜索和小窗口扫描完成, 不依赖粗扫直接给出最终精度。

### 2.2 MITP 候选选择改为“局部谷底 + DC 比较”

此前 MITP 候选会先受“一阶功率必须接近最深谷底”的限制, 导致一些 DC 更低但一阶稍浅的真实候选被排除。

当前策略为:

```text
先找所有合法局部一阶谷底
再在这些谷底候选中优先选择 DC 更低的点
一阶功率主要作为候选形状和 tie-break 信息
```

这符合当前实验认知: I/Q 最终 MITP 不能只看一阶最小, 也不能只看 DC 最小, 而应先确认它是局部一阶谷底, 再用 DC 辅助判断分支。

### 2.3 脚本可自动保存数据并画图

新增或增强了以下脚本:

```text
tools/run_dpmzm_positive_branch_flow.py
tools/plot_dpmzm_flow_metrics.py
tools/run_dpmzm_flow_com8.bat
```

脚本会自动完成:

```text
lock stop
set dump metrics
pilot-open on
set I/Q/P = 0/0/0 V
auto coarse
auto fine
positive-branch restore
P/I/Q/P turning-point small scans
pre-lock I/Q recheck
lock start
保存 serial log
保存 metrics CSV
绘制 all-stages PNG
```

## 3. 2026-05-08 新调制器实验观察

更换调制器后, 完整自动扫描暴露出隐式解对平台点判断的影响。代表性结果:

```text
run id: 2026-05-08_163639
final: I=-1.280 V, Q=+3.710 V, P=+2.640 V
lock probe: center best=no, direction=hold-weak
```

关键日志:

```text
I MATP candidates: +1.000 V +5.000 V
I plateau center: +3.750 V

Q MATP candidates: -4.000 V +1.500 V
Q plateau center: -0.500 V

P-QTP best: +0.000 V

i-MITP candidates: +1.000 V ... +5.000 V ... -> select +1.000 V
q-MITP candidates: +1.500 V ... -3.500 V ... -> select +1.500 V
```

这轮结果说明:

- I/Q MATP 粗扫中存在多个一阶极小候选, 其中包含隐式解或非目标分支。
- 当前“相邻 MATP 谷底之间找平台中心”的方法会被隐式解污染。
- 一旦 I/Q 平台中心错误, 后续 P-QTP 粗扫会基于错误 I/Q 初值进行, P 路可能找到局部可用但整体不优的点。
- 最终进入闭环时 P 路中心点不是三点中最优, 只能 `hold-weak`, 说明锁点质量不足。

## 4. 对 MATP 平台点的重新认识

此前流程隐含了一个过强假设:

```text
I/Q MATP 相邻谷底之间的一阶高平台
等价于 P 路交调项最高灵敏工作区
```

当前理论和实验都表明这个假设不可靠。

P 路 QTP 依赖的是 `fI + fQ` 或 `fI - fQ` 交调项, 而不是单独的 `fI` 或 `fQ`。更接近的关系是:

```text
P sensitivity ~= I pilot response * Q pilot response * P phase response
```

因此:

- `I` 单路一阶功率高, 不代表 `I/Q` 组合对 P 路一定灵敏。
- `Q` 单路一阶功率高, 也不代表 P-QTP 谷底一定清晰。
- 平台点只能说明该路被推离了 MATP 盲区, 不能直接证明它是 P 路最高灵敏度点。
- 隐式解会产生假谷底或假平台, 导致相邻谷底之间的平台中心失真。

更准确的定义应为:

```text
MATP 粗扫不是直接给出 P 路最优灵敏点。
MATP 粗扫只负责生成若干 I/Q 初始候选种子。
真正适合 P-QTP 的 I/Q 组合必须用 2200 Hz 交调项短扫验证。
```

## 5. 下一轮大改代码方向

### 5.1 分离两个任务

下一版应明确区分两个任务:

```text
任务 A: 为 P-QTP 生成 I/Q 高可观测候选种子
任务 B: 为最终 MITP 选择 I/Q 最小点
```

这两件事不能再混用同一套判据。

### 5.2 P-QTP 前的 I/Q 初值选择

旧逻辑:

```text
MATP-I 找谷底 -> 找平台中心
MATP-Q 找谷底 -> 找平台中心
直接用这一组 I/Q 扫 P-QTP
```

建议新逻辑:

```text
MATP-I 提取多个平台/推离候选
MATP-Q 提取多个平台/推离候选
组合出若干 I/Q 种子
对每组 I/Q 做短 P-QTP 验证
选择 2200 Hz 谷底更深, 对比度更大, 曲线形状更可信的组合
再进入正式 P-QTP / MITP 流程
```

也就是说, 平台点从“唯一答案”降级为“候选种子”。

### 5.3 I/Q 最终 MITP 选择

I/Q 最终 MITP 继续使用:

```text
必须是局部一阶谷底
在合法谷底候选中优先选择 DC 更低者
若 DC 接近, 再比较一阶功率深度和曲线形状
```

这适用于最终 I/Q 最小点, 不适用于 P-QTP 初始平台点。

### 5.4 需要保留的安全机制

大改时应保留以下已验证有效的机制:

- `auto coarse` 从 I/Q/P = 0/0/0 V 开始, 避免继承上次偏压状态。
- P-QTP 继续使用较高 blocks, 当前固件为 `P blocks = 10`。
- 扫描结束后继续做 pre-lock I/Q 小窗口复查。
- 闭环继续保留中心点保护和 `hold-weak` 逻辑。
- 脚本继续保存 serial log, metrics CSV 和 all-stages PNG。

## 6. 下一步实验建议

在大改前, 当前版本作为 checkpoint 保存。下一步可以先实现脚本或固件原型:

```text
1. 扫 MATP-I / MATP-Q
2. 每路提取 2~3 个平台候选或推离候选
3. 枚举 I/Q 组合
4. 每组做短 P-QTP 扫描
5. 用 2200 Hz 谷底深度, 谷底对比度, 边缘风险评分
6. 选择最可靠组合进入现有 auto fine / 小窗口复查
```

这次改动的核心目标不是把算法做复杂, 而是把“看起来像平台”替换成“实测 P 路确实敏感”。

## 7. 当前 checkpoint 的意义

本 checkpoint 用于保存大改前的稳定基线:

- 固件可以正常编译。
- 脚本可以一键扫描, 保存数据, 画图。
- 当前控制链路能运行, 但在存在隐式解的新调制器上会出现平台候选误判。
- 下一阶段代码重构将集中在候选生成和 P-QTP 灵敏度验证, 而不是串口, 导频, ADC 或闭环基础设施。
