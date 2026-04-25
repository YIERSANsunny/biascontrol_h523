# DPMZM 最小-最小-正交偏压控制方案（第9节修订来源）

> **文档身份**
> - 文档角色：历史 / 修订来源
> - 当前状态：历史文档
> - 默认优先阅读：否
> - 解决的问题：保留对整理版第 9 节及之后内容的修订思路来源
> - 关联数据：`../原始数据/SweepP.csv`、`../原始数据/Sweep_I_MITP.csv`
> - 关联图片/脚本：`../Python脚本/plot_sweep_p.py`、`../Python脚本/plot_sweep_i_mitp.py`、`../仿真图片/SweepP_曲线图.png`、`../仿真图片/Sweep_I_MITP_曲线图.png`
> - 被哪个主文档引用：`./理论知识总览.md`

> 说明：本文在 `DPMZM_最小最小正交偏压控制方案_整理版.md` 的基础上，保留整体控制框架，并重点修正“第三步：寻找 I 路和 Q 路 MITP”中的推导表述。  
> 其有效修订内容现已吸收到 `DPMZM_最小最小正交偏压控制方案_整理版.md` 中，本文保留为修订来源和历史回溯文档。  
> 本文特别澄清两件事：
> 1. 将另一条子 MZM 放在 MATP 作为参考点，这个思路本身是正确的。
> 2. 原整理版最后一步的问题，不在于“Q 路放在 MATP”，而在于：
>    - 把 P 路真实 QTP 近似成理想 `90°` 后，没有明确说明该近似只适用于参考推导；
>    - 在求解时没有把 MATP 分支和 MITP 分支区分开，导致最后给出的 MITP 近似式不够准确。

---

## 1. 方案目标

本文讨论一种基于双导频注入与光电反馈检测的 DPMZM（Dual-Parallel Mach-Zehnder Modulator）偏压控制方案。

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

DPMZM 输出光经 9:1 光耦合器分光，取小功率一路送入光电探测器（PD），将光功率变化转换为电信号，供数字控制模块提取各频率分量并完成偏压控制。

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
\beta = \pi \frac{V_Q}{V_{\pi,Q}}
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

## 6. 总体控制思路

从工程实现角度看，整个控制过程可分为三个主要阶段：

- **阶段 1：I/Q 粗定位**
  先利用基频分量扫描 I 路和 Q 路，寻找 MATP 作为绝对参考点。
- **阶段 2：P 路正交点搜索**
  利用导频交调项 $(f_I \pm f_Q)$ 搜索 P 路真实 QTP。
- **阶段 3：I/Q 细调到 MITP**
  在 P 路已经位于真实 QTP 后，再分别寻找 I 路和 Q 路的 MITP。

这里要特别强调：

> 第三步中“MITP 的真实位置”与 P 路真实 QTP 是否偏离理想 `90°` 密切相关。因此，第三步既可以讨论“参考推导下的名义 MITP”，也必须区分“当前非理想链路中的真实 MITP”。

---

## 7. 第一步：寻找 I 路和 Q 路 MATP

为后续建立相位参考坐标，先分别寻找 I 路和 Q 路的 MATP。以下以 Q 路为例说明。

由 Q 路基频分量主导项可得：

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

- 先寻找一阶信号的局部极小点
- 再判断该点附近功率变化曲线是否近似关于该点对称
- 若满足对称性，则可将该点判定为 MATP 候选点

I 路 MATP 的寻找方法同理。

### 7.2 非理想情况说明

当对应相位不接近 0 时，理想项占主导，曲线近似保持偶对称。
当相位接近 0 时，非理想项影响增强，极小点可能发生偏移。因此在实际扫点时：

- 不要求另外两路必须预先锁到某个固定点
- 但要避开导致曲线严重畸变或失去对称性的特殊区域
- 必要时可先微调另外两路，使扫描曲线恢复可识别性

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

### 8.1 工程说明

- I/Q 的 MATP 适合用于粗定位与建立相位参考
- 但寻找 P 路 QTP 时，I/Q 更适合停在对交调项仍有灵敏度的邻域
- 工程上更推荐提取同步检波后的带符号误差信号，而不是只看功率最小值

### 8.2 当前文档中的关键修正

P 路真实 QTP 在理想模型中位于 `90°`，但在存在器件失配、有限消光比和耦合不平衡等非理想项时，真实 QTP 可以偏离 `90°`。

因此：

- `90°` 只能作为**理想参考值**
- 当前非理想链路中的实际 MITP 搜索，应当以**真实 QTP** 为前提

---

## 9. 第三步：寻找 I 路和 Q 路 MITP（修正版）

本节是本文相对于整理版的核心修正部分。

### 9.1 先明确：Q 路放在 MATP 是正确前提

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

因此：

> 原整理版第 9 步的问题，不在于“Q 路放在 MATP”，这一步本身是成立的。

真正需要谨慎的是下一步对 P 路的处理方式。

### 9.2 若进一步把 P 路近似按理想 `90°` 处理

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

### 9.4 对原整理版错误的明确说明

原整理版第 9 节和第 12 节给出了：

$$
\sin(\phi_I) = \frac{2\delta_I}{1 + 2(\delta_I + \delta_P)}
$$

$$
\sin(\phi_Q) = -\frac{2\delta_Q}{1 + 2\delta_Q}
$$

并将其直接作为 MITP 近似解。

这一写法的问题在于：

1. 它没有把 **MATP 分支** 和 **MITP 分支** 区分开；
2. 它把从半角方程得到的小量分支，写成了整角正弦形式；
3. 它容易让人误以为“MITP 本身靠近 `0` 的小量解”，这在物理上是不对的。

因此，更准确的结论应为：

- 在“Q 路放 MATP、P 路按理想 `90°` 近似”的参考推导下：
  - **MITP 分支** 仍然在 $\phi=\pi$
  - **MATP 分支** 才对应半角形式的小量根

### 9.5 为什么当前 VPI 中 I 路最小点会是 2.4 V

上面的两支解，只适用于 **参考推导**。它们并不能直接描述当前非理想 VPI 工况中的真实 MITP。

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

### 11.1 带符号误差优先

如果只检测频率分量功率最小值，则适合扫点，但不利于稳态闭环。更推荐提取：

- I 路误差：$e_I$
- Q 路误差：$e_Q$
- P 路误差：$e_P$

使三路误差在目标点附近均表现为“过零型”信号。

### 11.2 不要把理想目标电压硬编码成最终工作点

- `2.5 V` 只适合理想 MITP 参考
- `90°` 只适合理想 QTP 参考
- 实际控制应锁定当前链路中的**真实误差零点**

### 11.3 明确区分“参考推导”和“真实工作点”

本文建议把两类结论严格分开：

- **参考推导结论**
  用于说明理想情况下的分支结构和名义点位
- **真实工作点结论**
  用于说明当前非理想链路中扫描或闭环得到的实际点位

---

## 12. 最终结论

围绕“最后一步寻找 I/Q 两路 MITP”的推导，本文最终结论如下：

1. 将另一条子 MZM 放在 MATP 作为参考点，这个思路本身是正确的。
2. 若把 P 路进一步近似按理想 `90°` 处理，只能得到一个**参考推导模型**。
3. 在这个参考推导模型中，I/Q 两路都应出现 **两支解**：
   - 一支对应 MATP 分支
   - 一支对应 MITP 分支
4. 真正的 MITP 分支仍然位于：

$$
\phi_I = \pi, \qquad \phi_Q = \pi
$$

5. 原整理版最后一步的错误，不在于“Q 路放在 MATP”，而在于：
   - 没有明确说明 `P 路按理想 90° 处理` 只是近似参考条件
   - 没有把 MATP 分支和 MITP 分支分开
   - 从而把半角小量分支误写成了 MITP 的近似解
6. 当前 VPI 中观察到的 `2.4 V` 等结果，属于当前非理想链路中的真实 MITP 偏移，不能用参考推导中的理想分支直接替代。

因此，DPMZM “最小-最小-正交”偏压控制方案的最终理解应当是：

> **MATP 用于建立绝对参考点，QTP 需要在非理想链路中通过交调误差寻找真实零点，而 I/Q 两路 MITP 也必须在真实 QTP 条件下通过基频误差寻找真实零点；理想 `90°`、理想 `2.5 V` 只能作为参考值，不能直接当作最终工作点。**
