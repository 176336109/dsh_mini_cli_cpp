# dsh-mini C++

面向嵌入式 Linux 和 ARM64 车机的轻量编码 Agent。它用 C++20 实现顺序 Agent 循环，支持 DeepSeek 流式响应、工作区工具、MCP stdio 和 JSONL 会话续跑。行为对照项目为 [Rust dsh-mini](https://github.com/176336109/dsh_mini_cli)。

- 通过一个 CLI 运行单轮任务或交互会话。用 `--resume latest` 或会话 ID 继续。
- 内置文件读取、写入、精确替换、目录列表、文本搜索、路径匹配和 Linux shell 工具。
- Linux 用 Landlock 约束子进程写入，用进程组回收超时或取消的任务。也可配置 CPU 亲和性、每进程地址空间、工具输出和历史预算。
- 用 RAII 管理文件、管道、锁和进程。类型化消息驱动循环，流式回调负责展示，JSON 编解码集中在协议和持久化边界。

## 构建

需要 C++20 编译器、CMake 3.22+ 和 libcurl 开发包。CTest 测试使用 Python 3。CMake 优先查找已安装的 nlohmann/json，缺包时会下载固定版本并验证 SHA-256。

Ubuntu 或 WSL2 中运行以下命令。将构建目录放在 Linux 文件系统可以避免跨文件系统写入构建产物。

```sh
sudo apt-get install g++ cmake ninja-build python3 pkg-config \
  libcurl4-openssl-dev nlohmann-json3-dev

cmake -S . -B "$HOME/build/dsh-mini-cpp/release" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/build/dsh-mini-cpp/release" --parallel
ctest --test-dir "$HOME/build/dsh-mini-cpp/release" --output-on-failure
```

Windows 构建支持文件工具、会话和独立协议测试。安装 libcurl 后可通过 `-DDSH_WITH_DEEPSEEK=ON` 启用模型传输。Linux 提供 shell、MCP 子进程和 Landlock。默认沙箱需要 Landlock ABI 3，启动时会报告实际使用的 ABI。

## 快速体验

在项目目录中用 Mock 离线读取本 README，然后续跑同一会话。

```sh
DSH_CPP="$HOME/build/dsh-mini-cpp/release/dsh-mini"
"$DSH_CPP" --model mock '!read_file {"path":"README.md","limit":10}'
"$DSH_CPP" --model mock --resume latest '!list_dir {"path":"."}'
```

真实模型示例从已有的凭据文件读取密钥。也支持通过 `DEEPSEEK_API_KEY` 环境变量、数据目录的 `.env` 或用户目录下的 `~/.dsh-mini/.env` 提供密钥。

```sh
"$DSH_CPP" --model deepseek-flash --key-file /path/to/deepseek-key \
  --cwd /path/to/project '找出项目的测试入口'
```

常用选项包括 `--thinking`、`--base-url`、`--proxy`、`--agents-file`、`--skills-dir` 和 `--system-file`。完整参数见 `dsh-mini --help`。

## MCP

通过 `--mcp mcp.json` 接入本地 stdio 服务。下面的服务提供 `echo` 工具时，Agent 会以 `mcp__demo__echo` 注册它。

```json
{
  "servers": [
    {"name": "demo", "command": "/path/to/mock-mcp-server", "args": []}
  ]
}
```

## 与 Rust 版对照

在同一 Linux 环境构建两个版本，用固定输入检查工具结果、错误类别、事件顺序和最终文件内容。两个脚本都接受现成的 Rust 和 C++ 二进制。

- [离线行为对照](tests/parity.py) 用 Mock 驱动文件工具、Bash、MCP 和 Agent 循环。
- [双向续跑](tests/compatibility.py) 先由 Rust 写入会话，再交给 C++，最后由 Rust 继续，核对 JSONL 和序号。
- [真实模型对照](tests/compare_agent.py) 复用 Rust 仓库的 53 条 fixture 和断言，逐条记录两版的通过情况、步数、工具错误和 token 用量。用本地配置提供相同的 DeepSeek 模型和凭据，事先设置两个独立的测试目录。

```sh
python3 tests/parity.py --rust "$RUST_BIN" --cpp "$CPP_BIN" \
  --mcp-server "$RUST_MCP_BIN" --report out/offline-parity.json

python3 tests/compare_agent.py --runner "$RUST_REPO/evals/run_evals.py" \
  --rust "$RUST_BIN" --cpp "$CPP_BIN" --mcp-server "$RUST_MCP_BIN" \
  --model deepseek-flash --work "$HOME/build/dsh-compare/run-01" \
  --report out/agent-parity.json
```

## ARM64

目标 SDK 应提供 AArch64 GNU C++ 编译器和 sysroot，包含目标运行库及 libcurl 的开发文件。

```sh
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  --toolchain cmake/aarch64-linux-gnu.cmake \
  -DDSH_TOOLCHAIN_PREFIX=/opt/vehicle-sdk/bin/aarch64-linux-gnu \
  -DDSH_TARGET_SYSROOT=/opt/vehicle-sdk/sysroot
cmake --build build-arm64 --parallel
file build-arm64/dsh-mini
```

实机性能可用 [对照脚本](tests/benchmark.py) 测量二进制大小、启动时间、CPU 时间、峰值 RSS 和长会话行为。验收条件和完成情况见 [实施计划](PLAN.md)。

## 许可证

本项目采用 [MIT 许可证](LICENSE)。
