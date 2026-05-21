# Spec 08 - MZM + DPMZM 单板集成计划

## 1. 集成目标

当前 `dpmzm-open-loop` 分支已经完成 DPMZM 自动找点、PC 侧梯度闭环、固件侧 `dpmzm auto lock` 等功能；主线 `upstream/main` 则完成了源码目录重构、MZM 基线控制、DMA 优化和 CubeMX 生成代码入库。

本阶段目标不是直接把两个分支硬合，而是建立一个可回退、可编译、可逐步验证的集成路线：

1. 保护当前 DPMZM 可用版本。
2. 以主线 `src/` + `scripts/` 新架构为底座。
3. 迁移 DPMZM 模块到主线架构。
4. 在同一固件内同时保留 MZM 与 DPMZM 功能。
5. 第一版只要求运行时二选一，不要求 MZM 与 DPMZM 同时闭环。

## 2. 分支与回退点

| 名称 | 类型 | 作用 | 创建/使用方式 |
| --- | --- | --- | --- |
| `upstream/main` | 远端主线 | 干净主线基底，包含 `src/` / `scripts/` 新架构 | `git fetch upstream` 后只读引用 |
| `dpmzm-open-loop` | 当前功能分支 | DPMZM 已验证功能来源 | 保持可运行，不直接在此分支合主线 |
| `dpmzm-known-good-2026-05-21` | tag | 当前 DPMZM 可用状态的保险点 | 已推送到远端 |
| `integration/mzm-dpmzm-unified` | 集成分支 | 实际迁移与集成工作区 | 从 `upstream/main` 创建 |
| `integration-step-xx-*` | 临时 tag | 每个可编译阶段的回退点 | 每完成一个稳定阶段再打 tag |

建议命令：

```powershell
git push origin dpmzm-open-loop
git tag dpmzm-known-good-2026-05-21
git push origin dpmzm-known-good-2026-05-21
git switch -c integration/mzm-dpmzm-unified upstream/main
```

## 3. 文件迁移表

主线已经将旧目录收敛到 `src/`，因此 DPMZM 代码迁移时不保留旧目录结构。

| DPMZM 当前文件 | 集成后位置 | 处理策略 |
| --- | --- | --- |
| `app/inc/app_config_dpmzm.h` | `src/app/app_config_dpmzm.h` | 直接迁移，保留 DPMZM 参数默认值 |
| `app/src/app_config_dpmzm.c` | `src/app/app_config_dpmzm.c` | 直接迁移，检查与主线 `app_config.*` 是否重复 |
| `app/inc/app_main_dpmzm.h` | `src/app/app_main_dpmzm.h` | 直接迁移，作为 DPMZM app 入口声明 |
| `app/src/app_main_dpmzm.c` | `src/app/app_main_dpmzm.c` | 迁移后接入统一 UART shell，不直接接管主循环 |
| `control/inc/ctrl_auto_dpmzm.h` | `src/control/ctrl_auto_dpmzm.h` | 直接迁移 |
| `control/src/ctrl_auto_dpmzm.c` | `src/control/ctrl_auto_dpmzm.c` | 迁移后检查扫描流程依赖 |
| `control/inc/ctrl_lock_dpmzm.h` | `src/control/ctrl_lock_dpmzm.h` | 直接迁移 |
| `control/src/ctrl_lock_dpmzm.c` | `src/control/ctrl_lock_dpmzm.c` | 迁移后检查与 MZM PID/lock 资源是否冲突 |
| `control/inc/ctrl_measure_dpmzm.h` | `src/control/ctrl_measure_dpmzm.h` | 直接迁移 |
| `control/src/ctrl_measure_dpmzm.c` | `src/control/ctrl_measure_dpmzm.c` | 重点检查 ADS131M02 采样率、raw buffer、Goertzel 频点 |
| `control/inc/ctrl_scan_dpmzm.h` | `src/control/ctrl_scan_dpmzm.h` | 直接迁移 |
| `control/src/ctrl_scan_dpmzm.c` | `src/control/ctrl_scan_dpmzm.c` | 迁移后保留 `DPMZMCSV` / `DPMZMSUM` 输出兼容性 |
| `tools/run_dpmzm_*.py` | `scripts/run_dpmzm_*.py` | 路径迁移，更新脚本内 repo/path 引用 |
| `tools/plot_dpmzm_*.py` | `scripts/plot_dpmzm_*.py` | 路径迁移，输出目录保持原实验目录可配置 |
| `tools/*_com8.bat` | `scripts/*_com8.bat` | 路径迁移，后续可改为可选串口 |
| `docs/plan/spec-06*.md` | `docs/plan/active/` 或 `docs/plan/completed/` | 保留历史，不在第一阶段重排文档结构 |

## 4. 资源冲突表

| 资源 | MZM 主线使用情况 | DPMZM 分支使用情况 | 集成风险 | 第一阶段处理策略 |
| --- | --- | --- | --- | --- |
| DAC8568 | MZM bias + pilot 输出 | DPMZM I/Q/P bias + I/Q pilot | 通道映射可能冲突 | 建立统一通道表，运行时只启用一个模式 |
| ADS131M02 CH0 | Goertzel AC 指标 | DPMZM AC / Goertzel 指标 | 采样率、blocks、频点配置不同 | 先复用主线 ADC 驱动，DPMZM 保留独立测量配置 |
| ADS131M02 CH1 | DC 监控 | DPMZM PD DC 触发小窗复查 | DC 语义可能不同 | 文档中明确 DC 是光功率均值，不是偏压 DC |
| SPI DAC | 主线 DMA 优化 | DPMZM pilot 与 bias 更新 | SPI/DMA 同时写 DAC 可能冲突 | 单模式运行，统一 DAC 写入口 |
| TIM/DMA | 主线 DAC SPI1 DMA、USART1 TX DMA、WFI | DPMZM TIM + LUT + DMA pilot | DMA 通道和中断优先级冲突 | 以主线 CubeMX 配置为准，缺失再补 |
| UART shell | MZM 命令与状态输出 | DPMZM 扫描、raw、lock 日志输出 | 命令名和长日志阻塞 | 命令命名空间拆成 `mzm` / `dpmzm` |
| 主循环 | MZM app state machine | DPMZM app process / auto lock | 两套流程抢占主循环 | 增加运行模式，仅当前模式的 process 被调用 |

## 5. 统一入口与运行模式

第一版集成只做模式切换，不做并行闭环。

建议统一命令：

```text
mode idle
mode mzm
mode dpmzm

status
mzm status
dpmzm status
dpmzm auto lock
```

行为约束：

1. `mode idle` 停止 MZM/DPMZM 控制，只保留串口和基础状态。
2. `mode mzm` 只运行 MZM 控制流程。
3. `mode dpmzm` 只运行 DPMZM 控制流程。
4. `mzm ...` 命令不得修改 DPMZM 状态。
5. `dpmzm ...` 命令不得修改 MZM 状态。
6. 共享 DAC/ADC/TIM/DMA 只能通过统一 driver 层访问。

## 6. 测试顺序

| 阶段 | 目标 | 操作 | 通过标准 |
| --- | --- | --- | --- |
| 0 | 主线基线可编译 | `cmake --build build -j 8` | 无编译错误 |
| 1 | DPMZM 文件迁移后可编译 | 迁移 DPMZM app/control/scripts 后编译 | 无缺头文件、无重复符号 |
| 2 | 串口命令不冲突 | 上板执行 `status`、`mzm status`、`dpmzm status` | 三个命令都有明确输出 |
| 3 | MZM 单独运行 | `mode mzm` 后执行 MZM 原有流程 | MZM 旧功能不退化 |
| 4 | DPMZM 单独运行 | `mode dpmzm` 后执行 `dpmzm auto lock` | 能完成找点并进入闭环 |
| 5 | DPMZM 数据链路验证 | `dpmzm capture raw`、FFT、扫描曲线 | 频点、功率、曲线与当前分支一致 |
| 6 | 长时间稳定性 | DPMZM lock 10~20 min | 无 still running、无 UART 卡死、无明显失控 |

宿主机测试优先：

```powershell
ctest --test-dir build
```

板级冒烟顺序：

```text
status
mzm status
dpmzm status
dpmzm set pilot-open on
dpmzm capture raw ...
dpmzm auto lock
```

## 7. 实施步骤

### Step 1 - 冻结当前 DPMZM

确保 `dpmzm-open-loop` 已推送，并打 `dpmzm-known-good-2026-05-21` tag。该 tag 是之后所有大改的安全回退点。

### Step 2 - 建立集成分支

从 `upstream/main` 创建 `integration/mzm-dpmzm-unified`。后续所有迁移工作只在该分支进行。

### Step 3 - 迁移 DPMZM 纯新增模块

先迁移 DPMZM 专属文件，例如 `ctrl_auto_dpmzm.*`、`ctrl_scan_dpmzm.*`、`ctrl_measure_dpmzm.*`、`ctrl_lock_dpmzm.*`。此阶段不改主线 driver，不接入主循环，只保证编译依赖逐步补齐。

### Step 4 - 接入 app 与 UART 命令

将 `app_main_dpmzm.*` 接入统一 shell。保留 `dpmzm ...` 命令，不重命名既有实验命令，避免破坏已有脚本。

### Step 5 - 统一 driver 资源访问

逐项合并 DPMZM 对 `drv_board`、`drv_ads131m02`、`drv_dac8568`、`drv_callbacks` 的修改。原则是共享底层只保留一份，DPMZM 特定逻辑上移到 control/app 层。

### Step 6 - 编译和板级冒烟

每迁移一个子系统就编译一次。出现冲突时优先保持主线结构，回填 DPMZM 行为。

### Step 7 - 模式隔离验证

先验证 `mode mzm`，再验证 `mode dpmzm`。确认单模式可靠后，再讨论是否需要真正同时运行。

## 8. 不变量

1. 不直接编辑 `cubemx/Core` 和 `cubemx/Drivers` 生成文件，除非通过 CubeMX 重新生成并确认 diff。
2. 不让 app/control 直接调用 HAL。
3. 不让 MZM 与 DPMZM 同时写 DAC，除非先完成资源仲裁。
4. 不删除当前 DPMZM 的串口输出格式，尤其是 `DPMZMCSV`、`DPMZMSUM`、lock debug 日志。
5. 不在没有示波器/频谱/串口验证的情况下认定集成成功。
6. 每个可编译阶段都可以回退。

## 9. 第一阶段验收标准

第一阶段完成时，应该满足：

1. 固件基于 `upstream/main` 新结构编译通过。
2. MZM 与 DPMZM 代码都在同一固件内。
3. 可以通过命令切换 `idle` / `mzm` / `dpmzm`。
4. MZM 原功能不退化。
5. DPMZM `auto lock` 能复现当前 `dpmzm-open-loop` 分支的找点和闭环效果。
6. 长时间 DPMZM lock 不出现 TIM/DMA/SPI/UART 卡死。

## 10. 当前默认假设

1. 主线 `upstream/main` 是目录结构和 CubeMX 配置的权威来源。
2. DPMZM 分支是 DPMZM 控制算法和实验脚本的权威来源。
3. 第一阶段不实现 MZM 与 DPMZM 并行闭环。
4. 第一阶段不重构 DPMZM 算法，只迁移并保持行为一致。
5. 如果资源冲突无法快速消解，优先保留“可切换单模式运行”，不要为了同时运行牺牲稳定性。
