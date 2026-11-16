# v-wise-cloud-core

云端双链路远控网关（P0-P2 联调版）。C++17，**零外部依赖**，Windows / Linux 双平台通用。

对应设计文档：《[双链路远控设计方案.md](../双链路远控设计方案.md)》（第 9 章云端网关技术栈）。
实施与测试步骤：《[P0-P2实施与测试方案.md](../P0-P2实施与测试方案.md)》；**与 RPi 联调的端到端步骤见《[联调操作手册.md](../联调操作手册.md)》**。

## 功能（按阶段）

| 阶段 | 能力 | 模块 |
|---|---|---|
| P0 | 探测回应（RTT/丢包）+ 带宽 train 实测 rx_rate 回传 | `anchor_server` |
| P1 | 上行双发选收去重（先到先得，per-锚点先到/重复统计） | `anchor_server` + `dedup` |
| P2 | 下行择优/双发（DupHdr 封装 + 端点学习 + 模式热切换） | `downlink_sender` |
| 控制面 | MQTT（可选）：订阅 `vwise/+/link/status/up`，auto 模式决策 | `mini_mqtt` + `mini_json` |

## 构建

### Windows（MSVC，联调机标准路径）

**1. 安装工具链**（管理员 PowerShell，一次性）：

```powershell
# VS 2022 Build Tools（含 MSVC 编译器 + Windows SDK）
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
# 独立 CMake（若 Build Tools 的 VCTools 推荐项已含 CMake 可跳过）
winget install Kitware.Cmake
```

**2. 打开编译环境**：开始菜单搜索并启动 **"x64 Native Tools Command Prompt for VS 2022"**
（关键一步——该快捷方式注入了 `cl.exe`/`link.exe` 的环境变量，普通 cmd/PowerShell 找不到编译器）。

**3. 构建与运行**：

```bat
cd /d <路径>\v-wise-cloud-core
cmake -B build
cmake --build build --config Release

:: 产物及运行文件（www\、cloud.ini 由 POST_BUILD 自动拷贝）全部位于 build\Release\
cd build\Release
vwise_cloud_core.exe --selftest      :: 零网络冒烟自检
vwise_cloud_core.exe --config cloud.ini
```

**常见问题**：

| 现象 | 处理 |
|---|---|
| `'cmake' 不是内部或外部命令` | 未在 x64 Native Tools 提示符中运行，或未安装 Kitware.Cmake |
| `'cl' 不是内部或外部命令` | 同上——必须用 Native Tools 提示符 |
| `Building for: NMake Makefiles` + `nmake failed: no such file or directory` | ① VS Build Tools 未安装或 VCTools 工作负载没装上（`winget list Microsoft.VisualStudio.2022.BuildTools` 检查）；② 失败缓存必须先删：`Remove-Item -Recurse -Force build`，然后重跑；任意终端可改用 `cmake -B build -G "Visual Studio 17 2022" -A x64`（不依赖 Native Tools 环境） |
| 装有多个 VS 版本，CMake 选错 | `cmake -B build -G "Visual Studio 17 2022" -A x64` |
| 中文注释告警 | 无需处理：CMakeLists 已加 `/utf-8` |
| 想用 MinGW | 非本文档路径，但 CMakeLists 兼容：`cmake -B build -G "MinGW Makefiles"`（产物在 `build\` 下） |

### Linux（生产 PoP 部署同款二进制）

```bash
sudo apt install -y cmake g++
cd v-wise-cloud-core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 运行

```bash
# 自检（零网络环境冒烟：probe 回显 / 双发去重 / 下行双路到达）
./vwise_cloud_core --selftest

# 正式运行（cloud.ini 与可执行文件同目录）
./vwise_cloud_core --config cloud.ini

# 常用覆盖
./vwise_cloud_core --config cloud.ini --dl-mode dual     # off|best|dual|auto
./vwise_cloud_core --config cloud.ini --no-mqtt          # 无 broker 环境纯数据面
./vwise_cloud_core --config cloud.ini --no-web           # 关闭内嵌看板
```

### Web 看板（联调）

默认开启：构建/运行目录下执行后浏览器访问 `http://<PC-IP>:8080/`。

- 终端侧无需 MQTT：`vwise_duptest`（或 agent）每 2s 发 `TERM_STATS` 上报，看板显示双链路 RTT/丢包/带宽、上下行计数、模式徽章、事件日志、10min 曲线；
- 验收面板实时计算 P1-1/P1-2/P1-3/P2-1/P2-2/P2-3 判定（模式或清零动作后重新计数）；
- 控制台一键操作：下行模式 off/best/dual/auto、终端模式 single/dual_dup/failover/auto（经 CTRL 包下发）、清零统计、触发带宽 train；
- `/metrics` 输出 Prometheus 格式（生产出口）。
- Windows 防火墙需放行 TCP 8080 入站（另有 UDP 39001-39010）。

### 配置（cloud.ini）

```ini
[server]
anchors = 0.0.0.0:39001, 0.0.0.0:39002   ; 双锚点 = 模拟双 PoP
[mqtt]
enable = 0                                ; 1 = 接入 mosquitto/EMQX
[downlink]
mode = dual                               ; off | best | dual | auto
rate_pps = 50
```

## 统计行说明（周期打印）

```
[stats] up: unique=1000(+500) dup=980(+490, 49.5%) first0=890(+430) first1=110(+70) |
         anchor_rx=[1490,1500] probe_ack=[30,30] | dl: mode=dual sent=2000 [1000/1000]
```

| 字段 | 含义 |
|---|---|
| unique(+Δ) | 全局去重后唯一上行包（增量）——应等于终端逻辑发包数 |
| dup(+Δ, %) | 重复副本丢弃数；双发时 ≈ unique（双倍带宽的代价） |
| first0/first1 | 各锚点"先到"计数——双发择优时两者之和 = unique |
| anchor_rx | 各锚点收包总数（含重复） |
| probe_ack | 各锚点回应探测次数（终端 RTT/丢包依据） |
| dl sent[a0/a1] | 下行已发：总数 / 经锚点0 / 经锚点1 |

**验收关系**（无 netem 注入时）：`unique ≈ 终端发送的逻辑包数`，`dup ≈ unique`（双发），`first0+first1 ≈ unique`，终端去重后 `rx_down ≈ 云端 sent_total`。

## 目录

```
v-wise-cloud-core/
├── CMakeLists.txt
├── cloud.ini                 # 配置（与二进制同目录）
├── src/
│   ├── vwise_dup_protocol.h  # 与 SDK 共享线协议（scripts/check_protocol_sync.sh 校验）
│   ├── net.{h,cpp}           # 跨平台 socket（Winsock/POSIX）
│   ├── logger.h              # 零依赖日志
│   ├── dedup.h               # 选收去重（与 SDK dedup_engine 同算法）
│   ├── mini_json.h           # 控制面 JSON 解析
│   ├── mini_mqtt.{h,cpp}     # QoS0 MQTT 3.1.1 客户端（mosquitto 兼容）
│   ├── cloud_config.{h,cpp}  # ini 配置
│   ├── anchor_server.{h,cpp} # 锚点：探测回应/去重/统计/端点学习
│   ├── downlink_sender.{h,cpp} # 下行择优/双发
│   ├── cloud_server.{h,cpp}  # 组装层 + 状态 + auto 模式
│   ├── selftest.cpp          # --selftest 冒烟
│   └── main.cpp
```
