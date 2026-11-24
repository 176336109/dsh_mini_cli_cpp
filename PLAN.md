# dsh-mini C++ 实施计划

项目目录为 `D:\wks\dsh_mini_cpp`，Rust 对照工程为 `D:\wks\DSH_Mini`。对照提交为 `2d08b3cccc0db3e99d88fd18c75db99f72cb5665`。现有 Windows Debug 二进制已通过评测断言器 12/12 和离线冒烟 2 步、1 次工具调用。C++ 版以 Linux x86_64 开发验收和 Linux ARM64 车机交付为目标。

## 方案

- C++20、CMake、CTest。核心库由 CLI 装配。模型使用可替换适配器，工具使用注册表。请求同步借用历史与工具 schema，流式回调用于显示，完整响应驱动 step。
- JSONL 保持 Rust v4 子集的事件信封与字段。会话逐事件持久化，进程只维护历史投影与续跑状态。校验序号及未知必需事件。恢复时把未完成的工具调用标记为中断，副作用由用户决定是否重新发起。
- 文件工具从目录描述符解析路径。Linux 进程用 RAII 管理描述符、进程组与回收。shell 和 MCP 复用进程层，启动时验证 Landlock ABI。
- libcurl 负责 HTTP，SSE 解码器维护有上限的分帧缓冲。DeepSeek 思考模式的协议字段通过模型适配器回传。MCP stdio 跑完握手、工具发现和调用后注册原有命名格式。
- 日志上限、历史预算和资源配额分别测量。对比两边 release 产物在同一台设备、同样输入下的体积、启动、RSS、CPU 时间与长会话表现。

## TodoList

### M0 · 构建与基线

- [x] 建立 CMake 核心库、CLI、CTest 和 Linux x86_64、ARM64 的 CI 配置。
- [ ] 在 Linux x86_64 上构建并运行 Debug 和 Release。
- [x] 固定 Rust 对照提交并记录既有离线基线。
- [ ] 用目标 sysroot 交叉编译 ARM64 最小程序，在目标机执行并记录运行库依赖和 Landlock ABI。

### M1 · 离线 Agent 闭环

- [x] CLI 支持一次任务、交互模式及工作目录、会话目录、模型和步数配置。
- [x] Mock、工具表、read_file、Agent 循环和流式展示。
- [x] JSONL 增量投影、恢复旧日志、编号连续、拒绝未知必需事件、标记中断的工具调用。
- [x] CTest 验证读文件闭环恰好 2 步 1 次工具调用，第二次模型请求收到匹配的 call_id。覆盖空输入、未知工具、步数限额、恢复和中断。

### M2 · 本地工具与沙箱

- [x] 补齐 write_file、str_replace、list_dir、grep 和 glob，运行文件工具验证。
- [ ] 在 Linux 上运行 bash 工具验证。
- [ ] 在 Linux 上运行 Landlock、进程组、超时、取消和输出上限测试。
- [ ] 验证工作区外写入拒绝、符号链接边界、超时回收和大输出限额。
- [ ] 原版 evals/run_evals.py --smoke 通过 C++ 二进制。

### M3 · DeepSeek 与 MCP

- [x] DeepSeek HTTP/SSE、思考模式、Key 文件与环境变量、网关和代理。
- [x] 项目指令、附加提示与技能索引。
- [ ] 在 Linux 上验证 MCP stdio 握手、工具发现、调用与退出回收。
- [x] 本地假网关和流解码测试覆盖任意分块、中文跨块、交错的工具调用分片、异常断开和思考字段回传。

### M4 · 车机交付

- [x] 验证日志达到上限时 Agent 能继续执行，历史裁剪及续跑投影一致。
- [ ] 在 Linux 和目标车机上验证 CPU 和内存配额实际生效。
- [ ] ARM64 Release 实机部署与 Rust 同机性能对照。报告产物尺寸、依赖、启动、RSS、CPU 时间、长会话和配额。
- [ ] 手动触发真实模型评测，沿用原版 fixture 与断言，报告通过率、步数和 token。MCP 用例另备 mock-mcp-server。

## 验证命令

以下命令以 WSL2 为例。直接使用同一份源码，Linux 构建目录独立于源码。

```sh
cmake -S /mnt/d/wks/dsh_mini_cpp -B "$HOME/build/dsh-mini-cpp/debug" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$HOME/build/dsh-mini-cpp/debug" --parallel
ctest --test-dir "$HOME/build/dsh-mini-cpp/debug" --output-on-failure

python3 /mnt/d/wks/DSH_Mini/evals/run_evals.py --self-test
DSH_BIN="$HOME/build/dsh-mini-cpp/debug/dsh-mini" \
  python3 /mnt/d/wks/DSH_Mini/evals/run_evals.py --smoke
```

## 2026-11-24 验收记录

| 检查 | 结果 |
|---|---|
| Windows x64 Debug、Release | libcurl 8.23.0 构建各通过 5/5 项 CTest。包含离线循环、文件工具、SSE 分帧、历史及日志预算，以及本地 HTTP 网关。 |
| Rust 与 C++ 双向续跑 | 对已有 Rust 调试二进制运行 tests/compatibility.py。Rust → C++ → Rust 完成 3 轮、35 条事件，序号连续，全程复用 1 条系统消息。 |
| Linux x86_64 语法检查 | Clang 22 使用 Debian Linux 头文件检查全部 10 个 C++ 源文件。Linux 构建、链接与运行待验证。 |
| Linux 执行 | bash、Landlock、进程回收、MCP、配额和 C++ 版原评测冒烟需要可运行的 Linux 环境。 |
| ARM64 车机 | 目标 SDK/sysroot、设备访问、Landlock ABI 和设备资源预算待核实。真实模型评测按原版约定手动触发。 |

当前 Windows 账号的 wsl 状态、版本和发行版查询均返回“未安装适用于 Linux 的 Windows 子系统”。虚拟机平台已启用，当前账号未发现 WSL 应用包或发行版注册。
