# Spec 06G - DPMZM 第一版固件实现清单
> 状态：Implementation checklist draft  
> 用途：把 `Spec 06F` 的控制思路拆成第一版固件可以直接开干的任务清单  
> 适用范围：当前 `DPMZM` 板载连续导频、开环扫描代码基础上，新增“板上自动粗捕获”能力  

---

## 1. 这份清单解决什么问题

`Spec 06F` 解决的是：

- 整体控制思路是什么
- 为什么这么做
- 各阶段在物理上分别负责什么

这份 `Spec 06G` 只解决一件事：

- **第一版固件到底先实现哪些功能**

也就是说，这不是终版闭环清单，而是：

- 第一版可落地的板上自动化实现清单

---

## 2. 第一版范围边界

## 2.1 第一版要完成什么

第一版只做：

- `I/Q-MATP` 自动粗扫
- 自动提取 `MATP` 参考谷底
- 自动提取相邻 `MATP` 谷底之间的一阶功率高平台
- 自动确定 `P-QTP` 粗扫初值
- 自动执行 `P-QTP` 粗扫
- 自动执行 `I/Q-MITP` 粗扫
- 用 `dc_mean` 做 `MITP` 分支初判
- 输出板上自动粗捕获结果

一句话就是：

- **第一版 = 自动粗捕获**

## 2.2 第一版明确不做什么

第一版先不做：

- `P / I / Q` 自动细扫循环
- 小窗口重复收敛
- 真正闭环的小扰动锁定
- 多平台、多分支全局比较搜索
- 正 / 负 `Q` 分支最终归一评估

一句话就是：

- **第一版不做真正闭环，只做自动找初值**

---

## 3. 第一版统一判据

为避免第一版实现过重，判据统一收敛如下：

### 3.1 `I/Q-MATP`

- 扫 `I` 看 `mag_fI`
- 扫 `Q` 看 `mag_fQ`
- 识别局部极小值作为 `MATP` 候选

### 3.2 `P-QTP`

- 只看 `mag_fsum_2200`

说明：

- `200 Hz` 当前受低频干扰较重
- 在第一版固件里，`200 Hz` 只作为调试输出保留
- 不进入自动控制主判据

### 3.3 `I/Q-MITP`

- 扫 `I` 看 `mag_fI`
- 扫 `Q` 看 `mag_fQ`
- 对最强局部极小值候选，用 `dc_mean` 做同一扫描内相对比较

---

## 4. 第一版建议的板上流程

第一版固件建议固定为下面这条自动流程：

1. 自动扫 `I-MATP`
2. 自动扫 `Q-MATP`
3. 从 `I/Q` 的 `MATP` 谷底之间提取主高平台
4. 设置 `I/Q` 到主高平台中心
5. 自动扫 `P-QTP`
6. 自动设置 `P` 到粗 `QTP` 候选
7. 自动扫 `I-MITP`
8. 自动扫 `Q-MITP`
9. 用 `dc_mean` 给出 `I/Q-MITP` 分支初判
10. 输出最终粗捕获结果

如果 `P-QTP` 粗扫失败，再执行一次回退：

1. 切换到 `I` 或 `Q` 的备选高平台中心
2. 重扫一次 `P-QTP`

注意：

- 第一版最多做一次回退
- 不做无限尝试

---

## 5. 推荐新增模块

当前代码结构下，建议新增一个独立控制模块，而不是把所有自动化逻辑塞进已有扫描文件。

推荐新增文件：

- `control/inc/ctrl_auto_dpmzm.h`
- `control/src/ctrl_auto_dpmzm.c`

可选新增文件：

- `app/inc/app_dpmzm_auto.h`
- `app/src/app_dpmzm_auto.c`

推荐职责划分：

### 5.1 `ctrl_scan_dpmzm.*`

继续负责：

- 单次扫描执行
- 单点测量
- CSV / RAW / SUM 输出

不负责：

- 自动流程状态机
- 候选点筛选
- 平台提取

### 5.2 `ctrl_auto_dpmzm.*`

负责：

- 自动粗捕获状态机
- `MATP` 候选提取
- 高平台提取
- `P-QTP` 粗扫初值选择
- `MITP` 候选与 `DC` 分支初判
- 最终粗捕获结果汇总

### 5.3 `app_main_dpmzm.c`

负责：

- 串口命令接入
- 参数解析
- 调用 `ctrl_auto_dpmzm`
- 打印状态与结果

---

## 6. 第一版最小数据结构清单

## 6.1 `MATP` 候选

```c
typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    float symmetry_score;
    uint16_t sample_index;
    bool valid;
} dpmzm_matp_candidate_t;
```

用途：

- 记录 `I/Q-MATP` 扫描中识别出来的局部极小值

## 6.2 高平台描述

```c
typedef struct {
    float left_matp_v;
    float right_matp_v;
    float plateau_left_v;
    float plateau_right_v;
    float plateau_center_v;
    float plateau_power_score;
    bool valid;
} dpmzm_sensitive_region_t;
```

用途：

- 描述相邻 `MATP` 谷底之间的一阶功率高平台

## 6.3 `MITP` 候选

```c
typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    uint16_t branch_rank;
    bool selected_as_mitp;
    bool valid;
} dpmzm_mitp_candidate_t;
```

用途：

- 记录 `I/Q-MITP` 扫描中最强局部极小值及其 `DC` 分支判定信息

## 6.4 自动粗捕获结果

```c
typedef struct {
    float i_matp_ref_v;
    float q_matp_ref_v;
    float i_pqtp_init_v;
    float q_pqtp_init_v;
    float p_qtp_coarse_v;
    float i_mitp_coarse_v;
    float q_mitp_coarse_v;
    bool qtp_valid;
    bool i_mitp_valid;
    bool q_mitp_valid;
} dpmzm_auto_coarse_result_t;
```

用途：

- 向上层和串口统一输出粗捕获结果

## 6.5 自动状态机上下文

```c
typedef enum {
    DPMZM_AUTO_IDLE = 0,
    DPMZM_AUTO_SCAN_I_MATP,
    DPMZM_AUTO_SCAN_Q_MATP,
    DPMZM_AUTO_PICK_PLATEAU,
    DPMZM_AUTO_SCAN_P_QTP,
    DPMZM_AUTO_SCAN_I_MITP,
    DPMZM_AUTO_SCAN_Q_MITP,
    DPMZM_AUTO_DONE,
    DPMZM_AUTO_FAILED
} dpmzm_auto_state_t;
```

再配一个上下文结构，保存：

- 当前状态
- 当前配置
- 候选数组
- 当前结果
- 回退计数

---

## 7. 第一版算法清单

## 7.1 `MATP` 局部极小值提取

要做：

- 从扫描数组中找局部极小值
- 计算每个极小值的局部对称性
- 去掉边界噪声点
- 输出若干 `MATP` 候选

第一版建议：

- 先只保留前 `2~3` 个最可信候选

## 7.2 高平台提取

要做：

- 将 `MATP` 候选按偏压排序
- 取相邻两个谷底形成区间
- 在区间内搜索一阶功率高平台
- 计算平台中心和平台评分

第一版建议：

- 只提取一个主平台
- 最多保留一个备选平台

## 7.3 `P-QTP` 粗候选提取

要做：

- 基于当前 `I/Q` 高平台初值执行 `P-QTP` 粗扫
- 从 `2200 Hz` 曲线中找最深谷底
- 同时记录次深谷底，供必要时调试

第一版建议：

- 自动设置到主谷底
- 只在曲线不可用时回退一次

## 7.4 `MITP` 候选提取与 `DC` 分支初判

要做：

- 在 `I/Q-MITP` 粗扫中找局部极小值
- 先按一阶功率深度排序
- 取前 `2` 个候选
- 比较 `dc_mean`
- 选择 `dc_mean` 更小者作为当前 `MITP`

第一版建议：

- 只做“同一扫描内相对比较”
- 不做跨批次 `DC` 归一化

---

## 8. 第一版命令接口建议

建议新增一个最小命令：

```text
dpmzm auto coarse
```

作用：

- 触发完整第一版自动粗捕获流程

可选扩展参数：

```text
dpmzm auto coarse [blocks] [step]
```

但第一版更推荐先固定默认参数，避免接口过早复杂化。

建议新增状态命令：

```text
dpmzm auto status
```

用于打印：

- 当前状态机阶段
- 最近一次自动粗捕获结果
- 最近一次失败原因

---

## 9. 第一版日志输出建议

第一版必须保证“自动流程虽然板上执行，但人还能看懂”。

建议至少打印这些节点：

- `auto: scan i-matp start`
- `auto: scan i-matp done`
- `auto: scan q-matp start`
- `auto: plateau selected`
- `auto: set i/q plateau center`
- `auto: scan p-qtp start`
- `auto: p-qtp coarse point selected`
- `auto: scan i-mitp start`
- `auto: i-mitp branch selected`
- `auto: scan q-mitp start`
- `auto: q-mitp branch selected`
- `auto: coarse capture done`

最终建议输出一个总结构：

```text
[dpmzm-auto] coarse result
  I matp ref:   ...
  Q matp ref:   ...
  I pqtp init:  ...
  Q pqtp init:  ...
  P qtp coarse: ...
  I mitp:       ...
  Q mitp:       ...
```

---

## 10. 第一版失败条件建议

第一版需要能明确告诉上层“为什么失败了”。

建议先定义这些失败码：

- `NO_VALID_I_MATP`
- `NO_VALID_Q_MATP`
- `NO_VALID_I_PLATEAU`
- `NO_VALID_Q_PLATEAU`
- `P_QTP_CURVE_UNUSABLE`
- `NO_VALID_I_MITP`
- `NO_VALID_Q_MITP`
- `ADC_OR_SCAN_ERROR`

这样后续调试不会只看到一个笼统的 `FAILED`。

---

## 11. 第一版验证清单

第一版写完后，至少要验证下面几件事：

### 11.1 `MATP` 候选是否稳定

- 同一条件下连续执行两次 `auto coarse`
- `I/Q-MATP` 候选是否大体一致

### 11.2 高平台选择是否合理

- 自动选出来的平台中心
- 是否和现有人工读图结果同量级

### 11.3 `P-QTP` 粗候选是否可用

- 自动粗捕获得到的 `P`
- 能否作为后续 `MITP` 粗扫的可用起点

### 11.4 `MITP` 分支初判是否与人工判断一致

- 自动选出的 `I/Q-MITP`
- 是否与当前人工依据 `dc_mean` 的选择一致

---

## 12. 第一版完成标志

只有同时满足下面 4 条，才能说第一版做完：

1. 板上可以一条命令完成自动粗捕获
2. 结果能稳定输出 `I/Q/P` 三路粗候选
3. 与现有人工扫描结果大体一致
4. 失败时能给出明确失败原因

---

## 13. 第一版之后再做什么

第一版完成后，再进入第二版：

- 自动细扫
- 小窗口循环收敛

最后第三版再做：

- 小扰动有符号误差
- 真正闭环锁定

也就是说，当前最重要的工程节奏是：

- **先把“自动粗捕获”做出来**
- 不要一开始就把所有闭环能力一起做完
