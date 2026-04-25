# DPMZM 偏压控制固件采样率不匹配问题分析与修复报告

## 1. 问题背景与现象

在真实光链路开环实验（阶段 A）中，利用固件提供的扫描功能（`scan matp/qtp` 等命令）获取到的 CSV 结果表现出严重异常：
- `I-MATP` 曲线的 `mag_fI` 和 `mag_fQ` 数值极小（接近本底噪声）。
- 曲线形态布满噪声抖动，且幅度与预期的量级完全不符。

然而，通过提取相同扫描工况下的 **ADC RAW 裸数据**，并在 PC 侧使用 Python 脚本进行离线 FFT 分析时，能够清晰地观察到高达 0.8 Vpp 的主导频峰（1000 Hz / 1200 Hz）。这说明硬件信号链（外部信号源 -> 偏压板 -> 减法器 -> MZM -> PD -> TIA -> ADC）是正常的，ADC 也成功捕获到了信号，但固件在 DSP 层的处理出现了严重偏差。

## 2. 核心根因：硬件采样率与 DSP 软件配置脱节

问题的根本原因在于：**硬件 ADC 的实际物理采样率是 32 kSPS，而固件 DSP 层坚定地将其硬编码为 64 kSPS。** 这种采样率的错位导致了灾难性的频率提取漂移。

### 2.1 硬件真实输出采样率的推导
固件使用 TI ADS131M02 ADC。根据数据手册（Table 8-12），在 HR（High-Resolution）模式下，其主时钟频率 $f_{CLKIN} = 8.192 \text{ MHz}$，调制器时钟为 $f_{MOD} = f_{CLKIN} / 2 = 4.096 \text{ MHz}$。
当配置 OSR（过采样率）为 128 时，数据输出率 $f_{DATA}$ 为：
$$f_{DATA} = \frac{f_{MOD}}{OSR} = \frac{4096000}{128} = 32000 \text{ Hz} = 32 \text{ kSPS}$$

前人在 `drv_ads131m02.h` 中的注释存在计算错误，误将 OSR=128 等同于 64 kSPS，这可能来源于某些“经验验证”时的假象（例如主循环定时器更新时的混叠）。

### 2.2 DSP 频点计算灾难（Goertzel 失锁）
在 `dsp_types.h` 中，DSP 参数被硬编码如下：
```c
#define DSP_SAMPLE_RATE_HZ           64000
#define DSP_PILOT_PERIOD_SAMPLES     64    // 64000 / 1000 = 64
#define DSP_GOERTZEL_BLOCK_CYCLES    20
#define DSP_GOERTZEL_BLOCK_SIZE      1280  // 64 * 20 = 1280
```

Goertzel 算法在初始化（`goertzel_init`）时，计算目标频率 $f_{target}$（如 1000 Hz）对应的频率仓索引 $k$：
$$k_{software} = N \times \frac{f_{target}}{f_{s(software)}} = 1280 \times \frac{1000}{64000} = 20$$

然而，这 1280 个样本是以真实的 32 kSPS 速率由 DRDY 中断送入的。因此，滤波器（$k=20, N=1280$）实际监控的物理频率变成了：
$$f_{actual} = \frac{k_{software} \times f_{s(real)}}{N} = \frac{20 \times 32000}{1280} = 500 \text{ Hz}$$

对于 1200 Hz 导频，其被错误计算为监控 600 Hz。
**结论：** 导频明明在 1000 Hz 和 1200 Hz，但 Goertzel 滤波器正在死死盯着 500 Hz 和 600 Hz。这完美解释了为什么 CSV 中的谐波幅度只有本底噪声，而 RAW 数据却能看到强烈的信号。

## 3. 出现问题的代码位置

1. **`biascontrol_h523/dsp/inc/dsp_types.h`**
   - 错误硬编码了 `DSP_SAMPLE_RATE_HZ`、`DSP_PILOT_PERIOD_SAMPLES` 和 `DSP_GOERTZEL_BLOCK_SIZE`。
2. **`biascontrol_h523/drivers/inc/drv_ads131m02.h`**
   - 注释错误地将 `ADS131M02_CLK_OSR_128` 标记为 `64 kSPS`。
3. **`biascontrol_h523/drivers/src/drv_ads131m02.c`**
   - `ads131m02_init` 中的注释和 printf 日志错误地宣称配置为 `64 kSPS`。

## 4. 修复方案

需要全面更正采样率定义，使其与真实的 32 kSPS 匹配，从而让 Goertzel 滤波器重新对准正确的物理频率。

### 4.1 修改 DSP 类型定义 (`dsp_types.h`)
将采样率更新为 32000，并重新计算保持整数周期和 20 ms 块大小的常量：

```c
/** ADC sample rate in Hz (ADS131M02, OSR=128, HR mode, 8.192 MHz CLKIN) */
#define DSP_SAMPLE_RATE_HZ      32000

/** Pilot tone frequency in Hz */
#define DSP_PILOT_FREQ_HZ            1000

/** Samples per 1 kHz pilot cycle at 32 kSPS. Must stay integer for coherent detection. */
#define DSP_PILOT_PERIOD_SAMPLES     32

/** Number of full pilot cycles integrated by each Goertzel block.
 *  Using multiple cycles improves SNR for weak harmonic extraction while
 *  preserving integer-cycle coherence (no spectral leakage). */
#define DSP_GOERTZEL_BLOCK_CYCLES    20

/** Goertzel block size in samples.
 *  N = 32 samples/cycle * 20 cycles = 640 samples = 20 ms @ 32 kSPS. */
#define DSP_GOERTZEL_BLOCK_SIZE      (DSP_PILOT_PERIOD_SAMPLES * DSP_GOERTZEL_BLOCK_CYCLES)
```
*注：修改后块大小从 1280 缩小到 640，处理时间减半，但物理时间窗仍为 20 ms。*

### 4.2 修正 ADC 驱动注释与日志 (`drv_ads131m02.h` / `.c`)
修正误导性的注释和初始化打印，防止后续开发再次被误导。

**drv_ads131m02.h:**
```c
/*
 * OSR field: bits [2:0] of CLOCK register.
 * Encoding per ADS131M02 datasheet Table 8-12:
 *   0x00 → OSR=128  → 32 kSPS  @ 8.192 MHz CLKIN (HR mode, fMOD = 4.096 MHz)
 *   0x01 → OSR=256  → 16 kSPS
 *   0x02 → OSR=512  →  8 kSPS
 *   0x03 → OSR=1024 →  4 kSPS
 *   0x04 → OSR=2048 →  2 kSPS
 *   0x05 → OSR=4096 →  1 kSPS
 *   0x06 → OSR=8192 → 500 SPS
 *   0x07 → OSR=16384→ 250 SPS  (HR only)
 */
#define ADS131M02_CLK_OSR_128       0x0000  /* 32  kSPS @ 8.192 MHz, HR */
#define ADS131M02_CLK_OSR_256       0x0001  /* 16  kSPS */
// ... 依此类推
```

**drv_ads131m02.c (`ads131m02_init`):**
```c
    /* Configure CLOCK register:
     * - Both channels enabled
     * - High-resolution power mode
     * - OSR = 128 → 32 kSPS at 8.192 MHz CLKIN (code 0x00)
     */
    uint16_t clock_val = ADS131M02_CLK_CH0_EN | ADS131M02_CLK_CH1_EN |
                         ADS131M02_CLK_PWR_HR | ADS131M02_CLK_OSR_128;
// ...
    printf("[adc] configured: OSR=128, GAIN=1, 32kSPS\r\n");
```

## 5. 预期结果
完成上述修改并重新编译烧录固件后：
1. `mag_fI` 和 `mag_fQ` 的提取幅度将恢复到正确的量级（与 Python 离线 FFT 结果一致，约 0.4V 甚至更高）。
2. 扫描出的 `I-MATP`、`Q-MATP` 曲线将变得极其平滑，具备极高的信噪比。
3. `P-QTP` 交调项（fsum / fdiff）也能在对应频点被准确捕捉。