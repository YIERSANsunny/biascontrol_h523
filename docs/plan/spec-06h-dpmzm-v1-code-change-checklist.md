# Spec 06H - DPMZM 第一版代码改动清单
> 状态：Code change checklist draft  
> 用途：把 `Spec 06G` 进一步落到当前代码库，明确第一版自动粗捕获应该改哪些文件、先加哪些结构体/接口、哪些暂时不要动  
> 适用范围：当前 `DPMZM` 连续导频、开环扫描、串口命令已经打通的代码基础上，新增“板上自动粗捕获”第一版实现

---

## 1. 这份清单和 06F / 06G 的关系

- `Spec 06F` 负责回答：控制思路是什么
- `Spec 06G` 负责回答：第一版固件要做哪些能力
- 这份 `Spec 06H` 负责回答：**在当前仓库里，代码到底从哪里下手改**

它不是新的理论文档，也不是最终实现文档，而是：

- 第一版开工前的代码施工图

---

## 2. 当前代码边界

结合当前代码结构，`DPMZM` 相关职责已经大致分成三层：

### 2.1 应用入口层

- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\inc\app_main_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\src\app_main_dpmzm.c`

当前已负责：

- `dpmzm` 串口命令解析
- 偏压状态保存
- 板载连续导频开关与状态显示
- `scan_begin / scan_end`
- `capture raw`
- 扫描命令到 `ctrl_scan_dpmzm` 的调用

当前还没有：

- 自动粗捕获状态机
- 自动流程结果结构
- `dpmzm auto ...` 命令族

### 2.2 单次扫描执行层

- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\inc\ctrl_scan_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_scan_dpmzm.c`

当前已负责：

- `MATP / QTP / MITP` 单次扫描执行
- 单点偏压切换、等待 `settle`
- 连续导频下整块采样
- Goertzel 指标提取
- `DPMZMCSV / DPMZMRAW / DPMZMSUM` 输出
- 返回一个简单 `summary`

当前还没有：

- 多轮自动流程编排
- 扫描曲线二次解释
- `MATP` 候选识别
- 高平台提取
- `MITP` 分支初判

### 2.3 单块测量层

- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\inc\ctrl_measure_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_measure_dpmzm.c`

当前已负责：

- `mag_fi`
- `mag_fq`
- `mag_fdiff`
- `mag_fsum`
- `dc_mean`

这一层当前已经够第一版用，**V1 不建议在这里继续加复杂解释逻辑**。

---

## 3. 第一版代码实现原则

### 3.1 不改坏现有扫描主链

第一版自动粗捕获应该尽量复用：

- 现有 `dpmzm_scan_request_t`
- 现有 `dpmzm_scan_run(...)`
- 现有 `app_dpmzm_scan_begin(...) / app_dpmzm_scan_end(...)`

也就是说：

- 自动粗捕获是“编排已有扫描能力”
- 不是重写一套新的采集 / 测量 / 导频链路

### 3.2 新增“解释层”，不要把解释逻辑塞回扫描执行层

建议做法是：

- `ctrl_scan_dpmzm.*` 继续只负责“扫”
- `ctrl_auto_dpmzm.*` 负责“解释扫描结果 + 决策下一步”

这样后面做第二版细扫和第三版闭环时，不会把 `ctrl_scan_dpmzm.c` 搞成大杂烩。

### 3.3 第一版 `P-QTP` 主判据只看 `2200 Hz`

自动主链统一只看：

- `mag_fsum`

也就是当前工程里的：

- `2200 Hz`

`200 Hz` 继续保留为调试输出，但不进入自动判断。

### 3.4 `MATP` 和高平台定义必须统一

当前工程语境下：

- `MATP`：`I/Q` 一阶功率最低的参考谷底
- `P` 路高灵敏工作区：相邻 `MATP` 谷底之间的一阶功率高平台

第一版实现里，绝对不能再把这两个概念混掉。

---

## 4. 推荐新增文件

第一版建议新增 2 个核心文件：

- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\inc\ctrl_auto_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_auto_dpmzm.c`

第一版**先不要**新增独立的 `app_dpmzm_auto.*`，原因是：

- 当前自动流程命令还很少
- 可以先直接由 `app_main_dpmzm.c` 调 `ctrl_auto_dpmzm`
- 等第二版命令和状态显示变复杂，再决定要不要把应用层再拆一层

---

## 5. 建议直接复用的现有文件

第一版自动粗捕获最应该复用这些文件：

- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\inc\app_main_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\src\app_main_dpmzm.c`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\inc\ctrl_scan_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_scan_dpmzm.c`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\inc\ctrl_measure_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\control\src\ctrl_measure_dpmzm.c`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\inc\app_config_dpmzm.h`
- `C:\Users\Administrator\Desktop\DPMZM_contral_bais\biascontrol_h523\app\src\app_config_dpmzm.c`

---

## 6. 第一版最小结构体清单

下面这些结构体建议第一批就定义好，避免后面边写边补。

### 6.1 自动流程状态枚举

```c
typedef enum {
    DPMZM_AUTO_IDLE = 0,
    DPMZM_AUTO_SCAN_I_MATP,
    DPMZM_AUTO_SCAN_Q_MATP,
    DPMZM_AUTO_PICK_I_PLATEAU,
    DPMZM_AUTO_PICK_Q_PLATEAU,
    DPMZM_AUTO_SCAN_P_QTP,
    DPMZM_AUTO_SCAN_I_MITP,
    DPMZM_AUTO_SCAN_Q_MITP,
    DPMZM_AUTO_DONE,
    DPMZM_AUTO_FAILED
} dpmzm_auto_state_t;
```

说明：

- `I/Q` 高平台分开列状态，比单一 `PICK_PLATEAU` 更容易打日志
- 第一版暂不需要更细的“细扫 / 闭环”状态

### 6.2 自动失败码

```c
typedef enum {
    DPMZM_AUTO_OK = 0,
    DPMZM_AUTO_ERR_BAD_ARG,
    DPMZM_AUTO_ERR_I_MATP_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_MATP_NOT_FOUND,
    DPMZM_AUTO_ERR_I_PLATEAU_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_PLATEAU_NOT_FOUND,
    DPMZM_AUTO_ERR_P_QTP_INVALID,
    DPMZM_AUTO_ERR_I_MITP_NOT_FOUND,
    DPMZM_AUTO_ERR_Q_MITP_NOT_FOUND,
    DPMZM_AUTO_ERR_SCAN_EXECUTION
} dpmzm_auto_error_t;
```

### 6.3 `MATP` 候选

```c
typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    float symmetry_score;
    uint16_t point_index;
    bool valid;
} dpmzm_matp_candidate_t;
```

### 6.4 高平台描述

```c
typedef struct {
    float left_matp_v;
    float right_matp_v;
    float plateau_left_v;
    float plateau_right_v;
    float plateau_center_v;
    float plateau_peak_dbm;
    float plateau_width_v;
    bool valid;
} dpmzm_sensitive_region_t;
```

### 6.5 `MITP` 候选

```c
typedef struct {
    float bias_v;
    float metric_f1_dbm;
    float dc_mean_v;
    uint16_t point_index;
    bool selected_as_mitp;
    bool valid;
} dpmzm_mitp_candidate_t;
```

### 6.6 粗捕获输入请求

```c
typedef struct {
    float sweep_min_v;
    float sweep_max_v;
    float sweep_step_v;
    uint32_t blocks;
    uint32_t settle_ms;
    bool use_onboard_pilot;
} dpmzm_auto_coarse_request_t;
```

说明：

- 第一版先统一一套粗扫参数
- 不要一开始就把每一路参数拆得太细

### 6.7 粗捕获结果

```c
typedef struct {
    float i_matp_ref_v;
    float q_matp_ref_v;
    float i_pqtp_init_v;
    float q_pqtp_init_v;
    float p_qtp_coarse_v;
    float i_mitp_coarse_v;
    float q_mitp_coarse_v;
    bool p_qtp_valid;
    bool i_mitp_valid;
    bool q_mitp_valid;
    dpmzm_auto_error_t error;
} dpmzm_auto_coarse_result_t;
```

### 6.8 自动流程上下文

```c
typedef struct {
    dpmzm_auto_state_t state;
    dpmzm_auto_error_t error;
    uint32_t retry_count;
    dpmzm_auto_coarse_request_t req;
    dpmzm_auto_coarse_result_t result;
} dpmzm_auto_context_t;
```

第一版先保持这个上下文很薄，避免过早把所有中间数组永久挂进全局结构体。

---

## 7. 第一版公共接口建议

建议在 `ctrl_auto_dpmzm.h` 里先定义这些接口。

### 7.1 初始化

```c
void dpmzm_auto_init(void);
```

### 7.2 查询状态

```c
const dpmzm_auto_context_t *dpmzm_auto_get_context(void);
```

### 7.3 执行一次自动粗捕获

```c
bool dpmzm_auto_run_coarse(const dpmzm_auto_coarse_request_t *req,
                           dpmzm_auto_coarse_result_t *out);
```

第一版建议先做成：

- **同步阻塞式一次跑完**

原因：

- 当前 `dpmzm scan ...` 本身就是阻塞式
- 板上自动粗捕获是“若干次阻塞扫描的顺序编排”
- 没必要第一版就强上后台状态机

### 7.4 结果打印辅助

```c
void dpmzm_auto_print_result(const dpmzm_auto_coarse_result_t *result);
```

第一版可以把打印函数也放在 `ctrl_auto_dpmzm.c`，后面若串口格式变复杂再上提。

---

## 8. 建议新增的内部辅助接口

这些函数第一版就值得在 `ctrl_auto_dpmzm.c` 里拆出来。

### 8.1 从扫描曲线提取 `MATP` 候选

```c
uint32_t dpmzm_auto_extract_matp_candidates(...);
```

职责：

- 从一阶曲线中找局部极小值
- 做简单对称性和边界过滤
- 输出前 `2~3` 个可信 `MATP` 候选

### 8.2 从 `MATP` 谷底之间提取主高平台

```c
bool dpmzm_auto_pick_sensitive_region(...);
```

职责：

- 按偏压排序 `MATP` 候选
- 取相邻谷底之间的区间
- 找该区间里的主高平台
- 输出一个主平台中心，必要时保留一个备选

### 8.3 从 `P-QTP` 扫描曲线取主谷底

```c
bool dpmzm_auto_pick_qtp_coarse_point(...);
```

职责：

- 只看 `2200 Hz`
- 选出最深谷底
- 给出粗 `P` 候选

### 8.4 从 `MITP` 曲线提取候选并做 `dc_mean` 判支

```c
bool dpmzm_auto_pick_mitp_branch(...);
```

职责：

- 找 `I/Q-MITP` 曲线中最强的两个局部极小值
- 在同一扫描内部比较 `dc_mean`
- 选 `dc_mean` 更小者作为当前 `MITP`

---

## 9. 对现有扫描层最小改动建议

第一版自动粗捕获会需要“拿到整条扫描曲线”，而不是只拿 `summary`。

因此对 `ctrl_scan_dpmzm.*` 最小但必要的增强建议是：

### 9.1 保留现有 `dpmzm_scan_run(...)`

这个接口不要删，也不要改成只服务自动流程。

### 9.2 新增“可选曲线缓存输出”

建议新增一个曲线点结构：

```c
typedef struct {
    float sweep_v;
    float mag_fi;
    float mag_fq;
    float mag_fdiff;
    float mag_fsum;
    float dc_mean;
} dpmzm_scan_point_t;
```

再新增一个扩展接口，例如：

```c
bool dpmzm_scan_run_collect(const dpmzm_scan_request_t *req,
                            dpmzm_scan_summary_t *summary_out,
                            dpmzm_scan_point_t *points,
                            uint32_t point_capacity,
                            uint32_t *point_count_out);
```

理由：

- 自动粗捕获需要完整曲线做二次解释
- 但现有手工扫描命令仍然可以继续走旧接口
- 这样新增能力最清晰，也最不容易伤到当前链路

### 9.3 不要把 `MATP/QTP/MITP` 解释塞进 `ctrl_scan_dpmzm.c`

`ctrl_scan_dpmzm.c` 应继续只负责：

- 扫
- 量
- 打印
- 返回原始曲线和摘要

而不是负责：

- 判断哪个谷底是 `MATP`
- 判断哪个平台适合 `P`
- 判断哪个 `MITP` 分支该选

---

## 10. 对应用入口层的改动建议

第一版直接在 `app_main_dpmzm.c` 里接入两个新命令就够了。

### 10.1 新增命令

```text
dpmzm auto coarse
dpmzm auto status
```

### 10.2 `dpmzm auto coarse` 的职责

- 解析可选参数；若没有参数，就用配置默认值
- 调用 `dpmzm_auto_run_coarse(...)`
- 打印阶段日志
- 打印最终结果
- 按策略把 `I/Q/P` 自动设置到粗捕获结果点

### 10.3 `dpmzm auto status` 的职责

- 打印当前自动流程状态
- 打印上次粗捕获结果
- 打印最近失败码

### 10.4 第一版不要改动的命令

以下命令保持原样：

- `dpmzm scan matp ...`
- `dpmzm scan qtp ...`
- `dpmzm scan mitp ...`
- `dpmzm capture raw ...`
- `dpmzm set bias ...`
- `dpmzm set pilot ...`
- `dpmzm set pilot-open ...`

这很重要，因为它们仍然是 bench 调试主工具。

---

## 11. 日志与结果输出建议

第一版自动流程一定要把日志打得比手工扫描更结构化。

建议阶段日志格式如下：

```text
[dpmzm][auto] state=SCAN_I_MATP
[dpmzm][auto] I-MATP candidates: -3.40V, +8.00V
[dpmzm][auto] I plateau center: +2.00V
[dpmzm][auto] state=SCAN_P_QTP
[dpmzm][auto] P-QTP best: +1.10V @ -84.6 dBm
```

最终结果建议至少打印：

- `I_MATP_ref`
- `Q_MATP_ref`
- `I_PQTP_init`
- `Q_PQTP_init`
- `P_QTP_coarse`
- `I_MITP_coarse`
- `Q_MITP_coarse`
- `error`

---

## 12. 推荐实施顺序

### 第 1 步：先加头文件和结构体

先完成：

- `ctrl_auto_dpmzm.h`
- 枚举、错误码、结果结构体

不要一上来就写完整流程。

### 第 2 步：给扫描层补“曲线回传能力”

先做：

- `dpmzm_scan_point_t`
- `dpmzm_scan_run_collect(...)`

只有拿到曲线，后面自动解释才有基础。

### 第 3 步：实现 `MATP` 候选提取

这是第一版自动粗捕获最基础的“解释层”。

### 第 4 步：实现高平台提取

从 `MATP` 候选推到：

- `I_pqtp_init`
- `Q_pqtp_init`

### 第 5 步：实现 `P-QTP` 粗点选择

只看：

- `2200 Hz`

### 第 6 步：实现 `I/Q-MITP` + `dc_mean` 判支

这一块不要复杂化，先只做同一扫描内相对比较。

### 第 7 步：接入 UART 命令

最后再把：

- `dpmzm auto coarse`
- `dpmzm auto status`

接到 `app_main_dpmzm.c`

---

## 13. 第一版暂时不要改的地方

为了控制风险，下面这些点第一版先别碰：

- 不改 `ctrl_measure_dpmzm` 的频点提取主逻辑
- 不把 `200 Hz` 拉回自动主判据
- 不实现后台非阻塞自动状态机
- 不实现自动细扫
- 不实现真正闭环小扰动
- 不引入多分支全局搜索
- 不把 `ctrl_scan_dpmzm.c` 改成“解释 + 扫描”混合文件

---

## 14. 第一版完成标志

如果下面这些都实现了，就可以认为 `06H` 对应的第一版代码施工完成：

- `dpmzm auto coarse` 可以一键跑完整个粗捕获流程
- 可以自动完成 `I-MATP -> Q-MATP -> P-QTP -> I-MITP -> Q-MITP`
- 自动流程内部只用 `2200 Hz` 作为 `P-QTP` 主判据
- `MITP` 可以基于 `dc_mean` 给出分支初判
- 自动结果可以打印并自动回写 `I/Q/P`
- 原有手工 `scan` / `capture raw` 命令不受影响

---

## 15. 下一份文档应该写什么

等 `06H` 对应代码真正开始动手后，下一份最值得补的文档应该是：

- `Spec 06I - DPMZM 自动粗捕获模块接口与状态机说明`

也就是把：

- 最终对外 API
- 命令参数
- 状态机阶段图
- 错误码含义
- 日志格式

单独整理出来，作为实现和调试的配套说明。
