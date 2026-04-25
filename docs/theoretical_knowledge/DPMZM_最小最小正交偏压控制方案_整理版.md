# DPMZM 最小-最小-正交偏压控制方案（整理版）

> **文档身份**
> - 文档角色：主理论
> - 当前状态：主文档
> - 默认优先阅读：是
> - 解决的问题：给出 DPMZM 最小-最小-正交偏压控制的总体理论框架、频率分量、控制顺序和当前统一标准结论
> - 关联数据：`../原始数据/隐式解为3.csv`、`../原始数据/SweepP.csv`、`../原始数据/Sweep_I_MITP.csv`
> - 关联图片/脚本：`../Python脚本/plot_yinshi_jie_3.py`、`../Python脚本/plot_sweep_p.py`、`../Python脚本/plot_sweep_i_mitp.py`、`../仿真图片/隐式解为3_曲线图.png`、`../仿真图片/SweepP_曲线图.png`、`../仿真图片/Sweep_I_MITP_曲线图.png`
> - 被哪个主文档引用：`./理论知识总览.md`、`../项目总控_DPMZM最小最小正交偏压控制.md`

> 说明：本文基于原始方案内容进行结构化整理，保留其核心思路，并对明显的符号不一致、表述跳跃和流程顺序进行了梳理。  
> 当前版本已经吸收 `DPMZM_最小最小正交偏压控制方案_第9节修订来源.md` 中关于第 9 节以后内容的修订，用于替代旧版中对 MITP 求解分支和真实 QTP 条件描述不清的问题。对仍需进一步仿真或实验校核的部分，文末单独列出。

---

## 1. 方案目标

本文讨论一种基于双导频注入与光电反馈检测的 DPMZM（Dual-Parallel Mach-Zehnder Modulator，双并行马赫-曾德调制器）偏压控制方案。

目标是将三路偏压分别稳定在以下工作点：

- **I 路子调制器**：MITP（最小传输点）
- **Q 路子调制器**：MITP（最小传输点）
- **P 路主调制器**：QTP（正交点）

对应的理想相移分别为：

$$
\phi_I = \pi, \qquad \phi_Q = \pi, \qquad \phi_P = \frac{\pi}{2}
$$

---

## 2. 系统结构

### 2.1 器件组成

主体器件为 DPMZM，其内部包含：

- 上路子调制器：I 路
- 下路子调制器：Q 路
- 主调制器：P 路

### 2.2 导频注入方式

在 I 路和 Q 路的直流偏置端分别叠加两个频率不同、幅度较小的导频信号：

- I 路导频：$f_I = V_I \sin(\omega_I t)$
- Q 路导频：$f_Q = V_Q \sin(\omega_Q t)$

### 2.3 反馈链路

DPMZM 输出光经 9:1 光耦合器分光，取小功率一路送入光电探测器（PD），将光功率变化转换为电信号，供单片机或数字控制模块提取各频率分量并完成偏压控制。

---

## 3. 相位定义与非理想因素

三路直流偏压引入的相位分别定义为：

$$
\phi_I = \pi \frac{V_{DC,I}}{V_{\pi,I}}, \qquad
\phi_Q = \pi \frac{V_{DC,Q}}{V_{\pi,Q}}, \qquad
\phi_P = \pi \frac{V_{DC,P}}{V_{\pi,P}}
$$

考虑器件失配、消光比有限、耦合不平衡等非理想因素，分别记为：

$$
\delta_I, \quad \delta_Q, \quad \delta_P
$$

导频注入引入的小信号相移分别为：

$$
\alpha = \pi \frac{V_I}{V_{\pi,I}}, \qquad
\beta  = \pi \frac{V_Q}{V_{\pi,Q}}
$$

因此三路总相移写为：

$$
\varphi_I = \phi_I + \alpha \sin(\omega_I t), \qquad
\varphi_Q = \phi_Q + \beta \sin(\omega_Q t), \qquad
\varphi_P = \phi_P
$$

---

## 4. DPMZM 输出模型

输入到 DPMZM 的归一化输出光功率可写为：

$$
I_{DPMZM} = \eta E_{out}(t) E_{out}^*(t) = I_{id} + I_{syn} + I_{nsyn} + o(\delta_k)
$$

其中：

- $I_{id}$：理想输出项
- $I_{syn}$：与理想项变化趋势一致的一阶同步扰动项
- $I_{nsyn}$：与理想项变化趋势不同的一阶非同步扰动项
- $o(\delta_k)$：更高阶小量，可忽略

在实际系统中，真正影响偏压判定精度的主导误差通常来自 $I_{nsyn}$。

### 4.1 理想输出项

$$
I_{id} = \frac{\eta P_{in}}{8}
\left[
2 + \cos(\varphi_I) + \cos(\varphi_Q)
+ 4\cos\left(\frac{\varphi_I}{2}\right)
\cos\left(\frac{\varphi_Q}{2}\right)
\cos(\varphi_P)
\right]
$$

### 4.2 同步扰动项

$$
I_{syn} = \frac{\eta P_{in}}{8}
\left[
\begin{aligned}
&2(\delta_I + \delta_Q + \delta_P) \\
&+ 2(\delta_I + \delta_P)\cos(\varphi_I) + 2\delta_Q\cos(\varphi_Q) \\
&+ 4(\delta_I + \delta_Q + \delta_P)
\cos\left(\frac{\varphi_I}{2}\right)
\cos\left(\frac{\varphi_Q}{2}\right)
\cos(\varphi_P)
\end{aligned}
\right]
$$

### 4.3 非同步扰动项

$$
I_{nsyn} = \frac{\eta P_{in}}{8}
\left[
\begin{aligned}
&4\delta_I \sin\left(\frac{\varphi_I}{2}\right)
\cos\left(\frac{\varphi_Q}{2}\right)
\sin(\varphi_P) \\
&- 4\delta_Q \cos\left(\frac{\varphi_I}{2}\right)
\sin\left(\frac{\varphi_Q}{2}\right)
\sin(\varphi_P)
\end{aligned}
\right]
$$

---

## 5. 可检测频率分量

PD 输出电信号中，控制模块主要提取以下三个频率分量：

1. **导频交调项**：$(f_I \pm f_Q)$
2. **I 路导频基频分量**：$f_I$
3. **Q 路导频基频分量**：$f_Q$

这三个分量分别对应 P 路、I 路、Q 路的偏压判定信息。

### 5.1 导频交调项 $(f_I \pm f_Q)$

$$
I_{I\_Q} = 2\mu\eta P_{in} \sin(\omega_Q t) \sin(\omega_I t)
\times
\left[
\begin{aligned}
&(1 + \delta_I + \delta_Q + \delta_P)
J_1\left(\frac{\alpha}{2}\right)
J_1\left(\frac{\beta}{2}\right)
\sin\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\cos(\phi_P) \\
&- \delta_I
J_1\left(\frac{\alpha}{2}\right)
J_1\left(\frac{\beta}{2}\right)
\cos\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P) \\
&+ \delta_Q
J_1\left(\frac{\alpha}{2}\right)
J_1\left(\frac{\beta}{2}\right)
\sin\left(\frac{\phi_I}{2}\right)
\cos\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P)
+ o(\delta_k)
\end{aligned}
\right]
$$

### 5.2 I 路基频分量 $f_I$

$$
I_I = \frac{\mu\eta P_{in} \sin(\omega_I t)}{16}
\times
\left[
\begin{aligned}
&-2(1 + 2(\delta_I + \delta_P))\sin(\phi_I) \\
&-4(1 + \delta_I + \delta_Q + \delta_P)
\sin\left(\frac{\phi_I}{2}\right)
\cos\left(\frac{\phi_Q}{2}\right)
\cos(\phi_P) \\
&+4\delta_I
\cos\left(\frac{\phi_I}{2}\right)
\cos\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P) \\
&+4\delta_Q
\sin\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P)
+ o(\delta_k)
\end{aligned}
\right]
$$

### 5.3 Q 路基频分量 $f_Q$

$$
I_Q = \frac{\mu\eta P_{in} \sin(\omega_Q t) J_1(m_Q)}{16}
\times
\left[
\begin{aligned}
&-2(1 + 2\delta_Q)\sin(\phi_Q) \\
&-4(1 + \delta_I + \delta_Q + \delta_P)
\cos\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\cos(\phi_P) \\
&-4\delta_I
\sin\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P) \\
&-4\delta_Q
\cos\left(\frac{\phi_I}{2}\right)
\cos\left(\frac{\phi_Q}{2}\right)
\sin(\phi_P)
+ o(\delta_k)
\end{aligned}
\right]
$$

---

## 6. 偏压搜索与控制思路

从工程实现角度看，整个控制过程可分为两个阶段：

- **粗定位阶段**：先建立 I/Q 两路的相位参考点
- **闭环锁定阶段**：利用三个误差信号分别锁定 P、I、Q 三路偏压

---

## 7. 第一步：寻找 I 路和 Q 路的 MATP（粗定位）

为后续建立相位参考坐标，先分别寻找 I 路和 Q 路的 MATP（最大传输点）。以下以 Q 路为例说明。

由 Q 路基频分量可知，其主导项来自理想分量展开。令理想主导项为零，可得：

$$
\sin\left(\frac{\phi_Q}{2}\right)
\left[
\cos\left(\frac{\phi_Q}{2}\right)
+
\cos\left(\frac{\phi_I}{2}\right)\cos(\phi_P)
\right] = 0
$$

在理想情况下，$\phi_Q = 0$ 是其中一个显式解，对应 MATP。且在 $2V_\pi$ 的扫描区间内一般会出现两个一阶信号极小点，因此需要结合曲线对称性来判定哪个点对应真正的 MATP。

### 7.1 识别原则

对 Q 路进行偏压扫描时：

- 先寻找 Q 路一阶信号的局部极小点
- 再判断该点附近功率变化曲线是否近似关于该点对称
- 若满足对称性，则可将该点判定为 Q 路 MATP 候选点

I 路 MATP 的寻找方法同理。

### 7.2 非理想情况说明

当 $\phi_Q$ 不接近 0 时，理想项 $I_{id}$ 占主导，响应曲线近似保持偶对称。
当 $\phi_Q$ 接近 0 时，非理想项的影响增强，极小点可能发生偏移。原始推导给出：

$$
\sin\left(\frac{\phi_Q}{2}\right)
=
\frac{-\delta_Q \cos\left(\frac{\phi_I}{2}\right) \sin(\phi_P)}
{\left[(1 + 2\delta_Q) + (1 + \delta_I + \delta_Q + \delta_P)A + \delta_I B\right]}
$$

其中：

$$
A = \cos\left(\frac{\phi_I}{2}\right)\cos(\phi_P), \qquad
B = \sin\left(\frac{\phi_I}{2}\right)\sin(\phi_P)
$$

这表明：Q 路一阶极小点会受 I 路和 P 路工作状态影响。因此在实际扫点时：

- 不要求 I 路和 P 路必须预先锁定在某个特定点
- 但要避开会导致曲线严重畸变或失去对称性的特殊区域
- 必要时可先微调 I 路和 P 路，使 Q 路扫描曲线恢复可识别的对称性

---

## 8. 第二步：寻找 P 路 QTP

在完成 I/Q 粗定位后，利用导频交调项 $I_{I\_Q}$ 来确定 P 路正交点 QTP。

其核心主导项为：

$$
I_{I\_Q} \propto
\sin\left(\frac{\phi_I}{2}\right)
\sin\left(\frac{\phi_Q}{2}\right)
\cos(\phi_P)
$$

因此，当 I、Q 两路处于对交调项具有足够灵敏度的区域时，可通过调节 $V_{DC,P}$，使交调项过零，从而找到 P 路 QTP。

### 8.1 关键说明

为了保证 P 路具有足够可观测性，应避免 I 路和 Q 路正好处于使
$\sin(\phi_I/2)$ 或 $\sin(\phi_Q/2)$ 过小的区域。

换句话说：

- I/Q 的 MATP 可用于**粗定位与建立相位参考**
- 但在寻找 P 路 QTP 时，I/Q 更适合处于对交调项仍有灵敏度的邻域
- 因此实际流程中，不建议让 I/Q 长时间停留在完全失去 P 路灵敏度的位置

### 8.2 P 路判定方法

通过扫描 P 路偏压并提取 $(f_I \pm f_Q)$ 分量：

- 观察交调项随 P 路偏压的变化
- 找到符号翻转或幅值过零位置
- 将该点作为 P 路 QTP

> 工程上建议使用**同步检波后的带符号误差信号**，而不是只取功率最小值。这样更适合后续闭环锁定。

---

## 9. 第三步：寻找 I 路和 Q 路 MITP（修正版）

在 P 路已锁定于 QTP 后，再分别细调 I、Q 两路至 MITP。

本节是当前主理论相对于旧整理版的核心修正部分。

### 9.1 先明确：另一条子 MZM 放在 MATP 是正确前提

若要推导 I 路 MITP，令 Q 路放在 MATP 作为参考点是合理的。此时：

$$
\phi_Q = 0, \qquad
\cos\left(\frac{\phi_Q}{2}\right)=1, \qquad
\sin\left(\frac{\phi_Q}{2}\right)=0
$$

将其代入 I 路基频表达式，可得：

$$
I_I \propto
\begin{aligned}[t]
&-2(1 + 2(\delta_I + \delta_P))\sin(\phi_I) \\
&-4(1 + \delta_I + \delta_Q + \delta_P)
\sin\left(\frac{\phi_I}{2}\right)\cos(\phi_P) \\
&+4\delta_I \cos\left(\frac{\phi_I}{2}\right)\sin(\phi_P)
\end{aligned}
$$

因此，旧版第 9 步的问题不在于“把另一条子 MZM 放在 MATP”，这一步本身是成立的。真正需要谨慎的是：后续若把 P 路真实 QTP 进一步近似成理想 `90°`，则必须把这种处理明确标注为**参考推导条件**，而不能直接当作当前非理想链路中的真实 MITP 条件。

### 9.2 参考推导：若进一步把 P 路近似按理想 `90°` 处理

为了得到一个简单、封闭的参考解，可以把 P 路真实 QTP 近似按理想正交点代入：

$$
\phi_P \approx \frac{\pi}{2}
\quad \Rightarrow \quad
\cos(\phi_P) \approx 0, \qquad \sin(\phi_P) \approx 1
$$

此时上式进一步化简为：

$$
I_I \propto
-2(1 + 2(\delta_I + \delta_P))\sin(\phi_I)
+ 4\delta_I \cos\left(\frac{\phi_I}{2}\right)
$$

令：

$$
A = 1 + 2(\delta_I + \delta_P)
$$

则零点条件为：

$$
-2A\sin(\phi_I) + 4\delta_I \cos\left(\frac{\phi_I}{2}\right)=0
$$

利用半角公式：

$$
\sin(\phi_I)=2\sin\left(\frac{\phi_I}{2}\right)\cos\left(\frac{\phi_I}{2}\right)
$$

得：

$$
4\cos\left(\frac{\phi_I}{2}\right)
\left[
\delta_I - A\sin\left(\frac{\phi_I}{2}\right)
\right] = 0
$$

因此应得到 **两支解**：

#### 解 1：MITP 分支

$$
\cos\left(\frac{\phi_I}{2}\right)=0
\Rightarrow
\phi_I = \pi + 2k\pi
$$

在主值范围内：

$$
\phi_I = \pi
$$

这才是 I 路的 **MITP 分支**。

#### 解 2：MATP 分支

$$
\sin\left(\frac{\phi_I}{2}\right)=\frac{\delta_I}{1 + 2(\delta_I + \delta_P)}
$$

在小失配条件下，该解位于 `0` 附近，对应的是 **MATP 一侧的分支**，而不是 MITP。

### 9.3 Q 路的对应结论

同理，若令 I 路处于 MATP，且将 P 路近似按理想 `90°` 处理，则：

$$
\phi_I = 0, \qquad
\cos\left(\frac{\phi_I}{2}\right)=1, \qquad
\sin\left(\frac{\phi_I}{2}\right)=0
$$

将其代入 Q 路基频表达式，可得：

$$
I_Q \propto
-2(1 + 2\delta_Q)\sin(\phi_Q)
- 4\delta_Q \cos\left(\frac{\phi_Q}{2}\right)
$$

令：

$$
B = 1 + 2\delta_Q
$$

则零点条件为：

$$
-2B\sin(\phi_Q) - 4\delta_Q\cos\left(\frac{\phi_Q}{2}\right)=0
$$

同样利用半角公式可得：

$$
4\cos\left(\frac{\phi_Q}{2}\right)
\left[
-B\sin\left(\frac{\phi_Q}{2}\right)-\delta_Q
\right]=0
$$

因此 Q 路同样有两支解：

#### 解 1：MITP 分支

$$
\cos\left(\frac{\phi_Q}{2}\right)=0
\Rightarrow
\phi_Q = \pi + 2k\pi
$$

主值范围内：

$$
\phi_Q = \pi
$$

#### 解 2：MATP 分支

$$
\sin\left(\frac{\phi_Q}{2}\right)
=
-\frac{\delta_Q}{1 + 2\delta_Q}
$$

该解位于 `0` 附近，对应 MATP 一侧分支。

### 9.4 对旧版错误的明确说明

旧版整理版曾给出：

$$
\sin(\phi_I) = \frac{2\delta_I}{1 + 2(\delta_I + \delta_P)}
$$

$$
\sin(\phi_Q) = -\frac{2\delta_Q}{1 + 2\delta_Q}
$$

并将其直接作为 MITP 近似解。

这一写法的问题在于：

1. 它没有把 **MATP 分支** 和 **MITP 分支** 区分开。
2. 它把从半角方程得到的小量分支，写成了整角正弦形式。
3. 它容易让人误以为“MITP 本身靠近 `0` 的小量解”，这在物理上是不对的。

因此，更准确的结论应为：

- 在“另一条子 MZM 放 MATP、P 路按理想 `90°` 近似”的参考推导下：
  - **MITP 分支** 仍然在 $\phi=\pi$
  - **MATP 分支** 才对应半角形式的小量根

### 9.5 为什么当前 VPI 中 I 路最小点会是 2.4 V

上面的两支解，只适用于**参考推导**。它们并不能直接描述当前非理想 VPI 工况中的真实 MITP。

原因是：

- 当前链路中的真实 QTP 不是理想 `90°`，而是例如 `93°`
- 有些扫描工况下，另一条子 MZM 也不一定严格停在 MATP
- 因此 `\cos(\phi_P)` 相关项和其他耦合项不会完全消失

例如，当 Q 路放在 MATP 而 P 路固定在真实 `QTP = 93°` 时，I 路零点条件应回到：

$$
I_I \propto
\begin{aligned}[t]
&-2(1 + 2(\delta_I + \delta_P))\sin(\phi_I) \\
&-4(1 + \delta_I + \delta_Q + \delta_P)
\sin\left(\frac{\phi_I}{2}\right)\cos(\phi_P) \\
&+4\delta_I \cos\left(\frac{\phi_I}{2}\right)\sin(\phi_P)
\end{aligned}
$$

若再考虑另一条子 MZM 不处于特殊点，则应进一步使用完整的 I 路基频表达式。

这说明：

> 当前 VPI 中出现的 `2.4 V`，不是“与主文档矛盾”的异常值，而是当前非理想链路在真实 QTP 条件下的 MITP 偏移结果。

---

## 10. 建议的闭环控制流程

基于上述修正，建议将控制流程整理为以下五步：

### 步骤 1：I/Q 粗扫描建立 MATP 参考点

- 分别扫描 I 路与 Q 路偏压
- 找到各自的一阶信号极小点
- 结合曲线对称性判断 MATP 候选点

### 步骤 2：利用交调项寻找 P 路真实 QTP

- 同时注入 I、Q 两路导频
- 提取 $(f_I \pm f_Q)$ 分量
- 扫描或闭环调节 P 路，使交调误差过零
- 记录当前链路中的真实 QTP

### 步骤 3：将另一条子 MZM 置于已知参考点

- 若寻找 I 路 MITP，则先将 Q 路固定在已知参考点
- 若寻找 Q 路 MITP，则先将 I 路固定在已知参考点

### 步骤 4：扫描并寻找真实 MITP

- 在当前真实 QTP 条件下扫描目标路偏压
- 提取对应基频分量
- 找到当前非理想链路中的真实零点

### 步骤 5：构建慢环闭环维持

- P 路使用较快环路维持真实 QTP
- I/Q 两路使用较慢环路维持各自真实 MITP
- 或采用轮询式更新：P → I → Q → P → I → Q

---

## 11. 工程实现建议

### 11.1 误差信号建议使用“带符号同步检波值”

如果只检测频率分量的功率最小值，则适合扫点，但不利于稳态闭环；更推荐采用同步检波或锁相放大方式直接提取：

- I 路误差：$e_I$
- Q 路误差：$e_Q$
- P 路误差：$e_P$

使三路误差在目标点附近均表现为“过零型”信号，以便构造稳定闭环。

### 11.2 导频幅度不宜过大

导频过大时，高阶贝塞尔项会增强，造成模型偏离小信号近似；导频过小时，信噪比又会下降。因此应通过实验选择合适的调制深度。

### 11.3 需要考虑三路耦合

虽然三个频率分量分别主要对应 P、I、Q 三路偏压，但在非理想条件下仍存在明显串扰。因此控制器设计中应考虑：

- 环路带宽分离
- 步进式轮询控制
- 增益限制与积分防饱和
- 温漂与 $V_\pi$ 漂移补偿

### 11.4 不要把理想目标值硬编码成最终工作点

- `2.5 V` 只适合理想 MITP 参考
- `90°` 只适合理想 QTP 参考
- 实际控制应锁定当前链路中的**真实误差零点**

---

## 12. 当前版本中已统一的关键结论

为避免旧版文本中的符号混乱，本文将当前真正可统一的关键结论写为：

1. 在参考推导条件下：

$$
\phi_I = \pi, \qquad \phi_Q = \pi
$$

分别对应 I 路和 Q 路的 **MITP 分支**。

2. 在同一参考推导下，MATP 一侧的小量根应写成半角形式：

$$
\sin\left(\frac{\phi_I}{2}\right)=\frac{\delta_I}{1 + 2(\delta_I + \delta_P)}
$$

$$
\sin\left(\frac{\phi_Q}{2}\right)
=
-\frac{\delta_Q}{1 + 2\delta_Q}
$$

3. 当前非理想链路中的真实 MITP 不应直接用理想 `2.5 V` 或参考推导分支替代，而应通过当前真实 QTP 条件下的基频误差零点来确定。

---

## 13. 仍需进一步核对的问题

虽然本文已完成结构整理，但以下内容仍建议在后续版本中进一步验证：

1. **I 路基频分量表达式中的贝塞尔项写法是否应显式写为 $J_1(m_I)$**
2. **MATP、MITP 的英文缩写及命名是否与全文保持一致**
3. **P 路 QTP 搜索时，I/Q 应停留的最优邻域范围需要通过仿真进一步给出**
4. **三路闭环在不同 $\delta_I, \delta_Q, \delta_P$ 条件下的稳定域需要补充数值结果**
5. **是否需要给出控制器框图、扫点流程图与锁定流程图**

---

## 14. 总结

该方案的核心思想是：

- 通过 I 路和 Q 路注入不同频率导频
- 利用 $f_I$、$f_Q$ 和 $(f_I \pm f_Q)$ 三个频率分量
- 分别构造 I 路、Q 路和 P 路偏压的观测量
- 最终将 DPMZM 稳定控制在“最小-最小-正交”工作状态

从理论框架上看，该方法具备较好的可实现性；从工程实现上看，关键在于：

- 正确建立 I/Q 的参考点
- 在 P 路搜索时保留足够的交调灵敏度
- 使用带符号误差而不是单纯功率最小值进行闭环
- 在 MITP 搜索中明确区分参考推导和真实工作点
- 通过环路解耦减弱三路耦合影响

只要进一步补足误差信号定义、闭环结构和仿真验证，这一方案可以整理为较完整的技术方案或论文基础稿。
