# DPMZM 偏压控制链路：各频率分量功率理论计算书

> **文档身份**
> - 文档角色：支撑说明
> - 当前状态：支撑文档
> - 默认优先阅读：否
> - 解决的问题：给出理想工作点附近各频率分量的理论电功率估算与 50Ω 系统换算关系
> - 关联数据：当前以理想工作点估算为主，不直接绑定某一组验证 CSV
> - 关联图片/脚本：暂无固定绘图脚本，主要服务于理论换算和频谱量级对照
> - 被哪个主文档引用：`./理论知识总览.md`、`./DPMZM_最小最小正交偏压控制方案_整理版.md`

> **文档说明**：本文档基于 DPMZM 在 **理想 MITP-MITP-QTP 工作点附近** 的参考模型，详细推导并计算 TIA 输出端直流(DC)以及 $1\text{ kHz}$、$1.2\text{ kHz}$、$2\text{ kHz}$、$2.2\text{ kHz}$ 频率分量的理论电功率（dBm）。  
> 本文档用于提供频率分量量级和换算关系的参考，不直接代表当前非理想 VPI 验证链路中的实际偏置点。

---

## 1. 系统基准参数与换算系数

首先，确立从光输入到 TIA 输出的系统级换算系数：

* **光输入功率**：$P_{in} = 10\text{ mW} = 0.01\text{ W}$
* **透射率**：单臂插入损耗为 $6\text{ dB}$，透过率 $L = 10^{-6/10} \approx 0.25119$
* **消光比系数**：$ER = 30\text{ dB}$，光功率串扰（漏光）系数 $\epsilon = 10^{-30/10} = 0.001$
* **光电增益**：
  * 耦合器分光比：$\eta_{tap} = 0.1$ (1:9 比例)
  * PD 响应度：$R = 0.8\text{ A/W}$
  * TIA 跨阻：$R_{TIA} = 100,000\text{ }\Omega$
* **全局换算系数 ($V_{base}$)**：
  $$V_{base} = \eta_{tap} \cdot R \cdot R_{TIA} \cdot \frac{P_{in} L}{4} = 8000 \times \frac{0.01 \times 0.25119}{4} \approx 5.0238\text{ V}$$

---

## 2. 相位调制深度与贝塞尔函数展开

在推挽模式（Dual-Drive，`LowerArmPhaseSense = NEGATIVE`）下，上下臂反相驱动，施加的射频电压会使总相移翻倍。

* **导频调制深度**（$V_{amp} = 0.025\text{ V}$，$V_{\pi} = 5\text{ V}$）：
  $$\alpha = \beta = \pi \frac{2 V_{amp}}{V_{\pi}} = \pi \frac{2 \times 0.025}{5} = 0.01\pi \approx 0.031416\text{ rad}$$

* **核心贝塞尔函数展开值**（利用小信号近似）：
  * $J_0(\alpha) \approx 1 - \frac{\alpha^2}{4} \approx 0.999753$
  * $J_1(\frac{\alpha}{2}) = J_1(0.005\pi) \approx \frac{\alpha}{4} \approx 0.007854$
  * $J_0(\frac{\beta}{2}) = J_0(0.005\pi) \approx 0.999938$
  * $J_2(\alpha) \approx \frac{\alpha^2}{8} \approx 0.0001234$

---

## 3. 光场非理想模型与电压分量提取

当 I 路和 Q 路精确偏置在最小传输点（MITP，$\phi_{DC} = \pi$）时，结合有限消光比 $\epsilon$，DPMZM 的归一化输出功率模型展开如下：

$$
P(t) \propto \left[ \underbrace{\sin^2\left(\frac{\alpha}{2}\sin\omega_I t\right) + \epsilon\cos^2\left(\frac{\alpha}{2}\sin\omega_I t\right)}_{\text{I路自乘项}} + \underbrace{\sin^2\left(\frac{\beta}{2}\sin\omega_Q t\right) + \epsilon\cos^2\left(\frac{\beta}{2}\sin\omega_Q t\right)}_{\text{Q路自乘项}} + \underbrace{2\sqrt{\epsilon}\sin\left(\frac{\alpha\sin\omega_I t - \beta\sin\omega_Q t}{2}\right)}_{\text{干涉串扰项}} \right]
$$

### ① 直流分量 (DC)
直流分量由 $P(t)$ 中的常数项构成：
* **I 路贡献**：$\frac{1 - J_0(\alpha)}{2} + \epsilon \frac{1 + J_0(\alpha)}{2} = \frac{1 - 0.999753}{2} + 0.001 \times \frac{1.999753}{2} \approx 0.001123$
* **Q 路贡献**：同为 $0.001123$
* **总中括号内系数**：$0.001123 \times 2 = 0.002246$
* **输出直流电压振幅**：$V_{DC} = 5.0238 \times 0.002246 = 0.01128\text{ V}$ ($11.28\text{ mV}$)

### ② $1\text{ kHz}$ 与 $1.2\text{ kHz}$ 基频分量 ($f_I, f_Q$)
理想 MITP 点基频应为 0，此处完全由 $30\text{ dB}$ 有限消光比导致的光场泄漏产生。取干涉串扰项展开：
* **展开式主导项**：$2\sqrt{\epsilon} \cdot 2J_1(\frac{\alpha}{2})\sin(\omega_I t) \cdot J_0(\frac{\beta}{2})$
* **系数幅度**：$4\sqrt{0.001} \times 0.007854 \times 0.999938 \approx 4 \times 0.031623 \times 0.007854 \times 1 \approx 0.0009934$
* **输出交流电压振幅**：$V_{1k} = V_{1.2k} = 5.0238 \times 0.0009934 = 0.00499\text{ V}$ ($4.99\text{ mV}$)

### ③ $2\text{ kHz}$ 二次谐波分量 ($2f_I$)
MITP 点的固有特征是偶次谐波最大化。从 I 路自乘项提取 $\cos(2\omega_I t)$ 前的系数：
* **展开式主导项**：$(\epsilon - 1)J_2(\alpha)$
* **系数幅度**：$|0.001 - 1| \times 0.0001234 \approx 0.0001233$
* **输出交流电压振幅**：$V_{2k} = 5.0238 \times 0.0001233 = 0.000619\text{ V}$ ($0.619\text{ mV}$)

### ④ $2.2\text{ kHz}$ 交调分量 ($f_I + f_Q$)
根据理论推导，交调项幅度正比于主调制器(P路)偏置相位的余弦值：$\cos(\phi_P)$。
* **计算结论**：当前 P 路精确锁定在正交点（QTP，$\phi_P = 90^\circ$），因此 $\cos(90^\circ) = 0$。该频率分量被完美抑制，理论值为 $0\text{ V}$。

---

## 4. 电功率 (dBm) 换算与汇总表

频谱仪将测得的电压换算为标准 $50\text{ }\Omega$ 负载上的消耗功率：
* **交流信号**（转换为有效值计算）：$P_{dBm} = 10 \log_{10}\left( \frac{V_{amp}^2}{2 \times 50} \times 1000 \right)$
* **直流信号**：$P_{dBm} = 10 \log_{10}\left( \frac{V_{DC}^2}{50} \times 1000 \right)$

代入上述算出的电压振幅：

| 频率 | 物理来源 | TIA 输出振幅 (mV) | 换算计算式 | 50Ω 系统功率 (dBm) |
| :--- | :--- | :--- | :--- | :--- |
| **DC** | 漏光底噪 + RF均值 | $11.28$ | $10 \log_{10}((0.01128^2 / 50) \times 1000)$ | **-25.9** |
| **1 kHz** | I 路基频 (ER 泄漏) | $4.99$ | $10 \log_{10}((0.00499^2 / 100) \times 1000)$ | **-36.0** |
| **1.2 kHz**| Q 路基频 (ER 泄漏) | $4.99$ | 同上 | **-36.0** |
| **2 kHz** | I 路二次谐波 (MITP) | $0.619$ | $10 \log_{10}((0.000619^2 / 100) \times 1000)$ | **-54.2** |
| **2.2 kHz**| I/Q 交调 (QTP 抑制)| $\approx 0$ | $10 \log_{10}(0)$ | **$-\infty$** |

> **注**：在实际 VPI 仿真频谱中，$2.2\text{ kHz}$ 处的功率值不会是绝对的 $-\infty$，而是会体现为仿真软件的截断误差本底噪声（通常低于 $-150\text{ dBm}$）。
