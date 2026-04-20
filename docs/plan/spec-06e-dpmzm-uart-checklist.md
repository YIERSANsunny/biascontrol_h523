# Spec 06E - DPMZM 上板 UART 命令清单
> 状态: Active working note  
> 用途: 作为明天上板实验时的直接执行清单  
> 适用范围: 当前这版 **DPMZM 开环实验固件**  

---

## 1. 使用前提

这份清单默认基于当前代码状态:

- DPMZM 命令已经通过 **临时桥接** 接入原串口入口
- 当前命令命名空间统一为:
  - `dpmzm ...`
- 当前目标是 **开环扫描**
- 当前不使用原来的:
  - `start`
  - `stop`
  - MZM 闭环命令链

当前 DPMZM 路径的主要命令有:

- `dpmzm status`
- `dpmzm set bias i <V>`
- `dpmzm set bias q <V>`
- `dpmzm set bias p <V>`
- `dpmzm set pilot-src onboard`
- `dpmzm set pilot i <freq_hz> <mVpp>`
- `dpmzm set pilot q <freq_hz> <mVpp>`
- `dpmzm set dump metrics|raw|both`
- `dpmzm scan matp i ...`
- `dpmzm scan matp q ...`
- `dpmzm scan qtp p ...`
- `dpmzm scan mitp i ...`
- `dpmzm scan mitp q ...`

---

## 2. 上电前检查

在真正发 `dpmzm` 命令前，先确认这几件事:

1. `I/Q/P` 三路 DAC 通道映射是否已经和当前真实接线一致  
   当前代码里的 `0/1/2` 仍是默认占位，不是最终硬件事实。

2. ADC 接线是否满足当前软件假设:
   - `CH0` = AC 响应
   - `CH1` = DC 参考

3. 光链路已经接通:
   - 激光
   - 偏振控制
   - DPMZM
   - PD
   - TIA

4. 默认导频源为板上 DAC 生成  
   当前不建议依赖外部双导频源。

5. 初次上电前，建议所有偏压先从 `0 V` 附近开始

---

## 3. 上电后的最小验证顺序

### 第一步：先看老主线有没有正常启动

先敲:

```text
status
```

目的:

- 确认板子已经正常启动
- 确认 UART 命令入口工作正常

### 第二步：看 DPMZM 临时入口是否正常

再敲:

```text
dpmzm
dpmzm status
```

预期:

- `dpmzm` 会打印用法提示
- `dpmzm status` 会打印当前:
  - `I/Q/P` 偏压
  - 导频源
  - 导频频率
  - 导频幅度
  - dump 模式

### 第三步：把三路偏压先置到安全初值

```text
dpmzm set bias i 0.0
dpmzm set bias q 0.0
dpmzm set bias p 0.0
dpmzm status
```

目的:

- 确认三路偏压命令生效
- 避免一上来就从未知偏压开始扫描

### 第四步：确认导频参数

```text
dpmzm set pilot-src onboard
dpmzm set pilot i 1000 50
dpmzm set pilot q 1200 50
dpmzm set dump metrics
dpmzm status
```

默认解释:

- `1000` / `1200` = `fI / fQ`
- `50` = `50 mVpp`
- `metrics` = 只打印每个扫点的摘要，不打印原始 ADC 样本

---

## 4. 推荐实验顺序

当前建议严格按下面顺序执行:

1. `I-MATP`
2. `Q-MATP`
3. `P-QTP`
4. `I-MITP`
5. `Q-MITP`

原因:

- `MATP` 先建立参考点
- `QTP` 依赖 `I/Q` 已进入可观测区
- `MITP` 依赖 `P` 已经落在真实 `QTP`

---

## 5. I 路 MATP 扫描

### 建议前置设定

先把 `Q/P` 固定在一个中间参考点:

```text
dpmzm set bias q 0.0
dpmzm set bias p 0.0
```

### 粗扫示例

```text
dpmzm scan matp i -3.0 3.0 0.1 6
```

字段解释:

- `matp` = 当前阶段
- `i` = 扫 I 路
- `-3.0 3.0` = 起止范围
- `0.1` = 步进
- `6` = 每个扫点积累 6 个测量块

预期:

- 串口输出 `DPMZMCSV`
- 最后输出 `DPMZMSUM`

如果曲线太噪，可以加块数:

```text
dpmzm scan matp i -3.0 3.0 0.1 10
```

---

## 6. Q 路 MATP 扫描

### 建议前置设定

先固定 `I/P`:

```text
dpmzm set bias i 0.0
dpmzm set bias p 0.0
```

### 粗扫示例

```text
dpmzm scan matp q -3.0 3.0 0.1 6
```

如果确认某个区间附近有候选点，再细扫，例如:

```text
dpmzm scan matp q -0.6 0.6 0.02 10
```

---

## 7. P 路 QTP 扫描

### 重要说明

做 `P-QTP` 扫描前，不要让 `I/Q` 正好停在 MATP 深谷点上，否则交调项会太弱。

也就是说，扫描前建议把 `I/Q` 从 MATP 轻微推离一点，让交调可见。

例如，如果某次 MATP 粗略在 `0.0 V`，可以先试:

```text
dpmzm set bias i 0.2
dpmzm set bias q 0.2
```

### 粗扫示例

```text
dpmzm scan qtp p 0.0 5.0 0.1 6
```

说明:

- 这里软件还不知道你的 `P` 电压和相位如何换算
- 所以当前是按“P 路偏压电压轴”扫描，不是直接输入“90 度”

预期:

- 主看 `DPMZMCSV` 里的 `mag_fsum`
- `DPMZMSUM` 会给出当前主判据下的最优候选点

如果粗扫找到候选区，再细扫:

```text
dpmzm scan qtp p 2.0 3.0 0.02 10
```

---

## 8. I 路 MITP 扫描

### 建议前置设定

先把 `P` 固定在刚才扫到的真实 `QTP` 候选点。  
`Q` 先固定在参考点。

例如:

```text
dpmzm set bias p 2.45
dpmzm set bias q 0.0
```

### 粗扫示例

```text
dpmzm scan mitp i -3.0 3.0 0.1 6
```

细扫示例:

```text
dpmzm scan mitp i 1.8 2.8 0.02 10
```

注意:

- 这条曲线上通常会同时看到 MATP 分支和 MITP 分支
- 最终要结合已知 MATP 位置来区分哪一支才是 MITP

---

## 9. Q 路 MITP 扫描

### 建议前置设定

先固定:

- `P` = 已知真实 `QTP`
- `I` = 已知参考点

例如:

```text
dpmzm set bias p 2.45
dpmzm set bias i 2.40
```

### 粗扫示例

```text
dpmzm scan mitp q -3.0 3.0 0.1 6
```

细扫示例:

```text
dpmzm scan mitp q 2.0 3.0 0.02 10
```

---

## 10. dump 模式怎么选

### 推荐默认

```text
dpmzm set dump metrics
```

适合:

- 明天先把曲线扫出来
- 串口负载较轻

### 只有在必要时才用

```text
dpmzm set dump raw
```

或

```text
dpmzm set dump both
```

适合:

- 需要离线复算
- 需要看单点原始 ADC 样本

注意:

- `raw/both` 会让串口输出量暴增
- 明天第一次上板不建议默认就开

---

## 11. 推荐最小执行模板

如果明天时间紧，只想先验证主链路，建议按这个最小模板来。

```text
status
dpmzm status
dpmzm set bias i 0.0
dpmzm set bias q 0.0
dpmzm set bias p 0.0
dpmzm set pilot-src onboard
dpmzm set pilot i 1000 50
dpmzm set pilot q 1200 50
dpmzm set dump metrics
dpmzm scan matp i -3.0 3.0 0.1 6
dpmzm scan matp q -3.0 3.0 0.1 6
dpmzm set bias i 0.2
dpmzm set bias q 0.2
dpmzm scan qtp p 0.0 5.0 0.1 6
```

如果这三条曲线能正常出来，说明:

- 三路偏压设置链路正常
- 板上双导频生成正常
- ADC/Goertzel 测量链路正常
- `I/Q MATP -> P QTP` 主实验路径已经打通

---

## 12. 当前最值得注意的风险

### 风险 1：DAC 通道映射还没冻结

如果 `I/Q/P` 的硬件接线和代码默认映射不一致，扫描结果会直接错位。

### 风险 2：P 扫描前 I/Q 停在了 MATP 深谷

这样会导致交调项太弱，`QTP` 曲线不明显。

### 风险 3：一上来就用 `raw/both`

串口输出太重，会影响明天快速验证。

### 风险 4：把当前电压轴误当成理论相位轴

当前 `scan qtp p ...` 还是“P 路偏压扫描”，不是“直接扫 90 度”。

---

## 13. 一句话使用原则

明天上板时，先不要追求一步到位闭环。

先按下面的顺序做:

`状态检查 -> 偏压归零 -> 导频确认 -> I/Q MATP -> P QTP -> I/Q MITP`

只要前 3 张曲线先出来，整条 DPMZM 开环实验链路就算真正开始跑起来了。
