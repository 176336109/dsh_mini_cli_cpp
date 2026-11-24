# dsh-mini C++

面向嵌入式 Linux 的轻量编码 Agent。核心循环顺序执行模型请求和工具调用，JSONL 会话支持续跑。项目主要面向 Linux x86_64 和 ARM64，Windows 构建用于验证可移植逻辑。

## 构建与离线验证

Linux 需要 C++20 编译器、CMake 3.22+、Python 3 和 libcurl 开发包。CMake 优先使用系统的 nlohmann/json。缺少系统包时，CMake 用固定 SHA-256 校验下载的 JSON 头文件。隔离构建可在 sysroot 中预装 nlohmann/json。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/dsh-mini --model mock '!read_file {"path":"README.md","limit":10}'
./build/dsh-mini --resume latest --model mock '!list_dir {"path":"."}'
```

Linux 默认使用 Landlock，要求 ABI 3 以保护目录外文件的截断操作。命令和 MCP 子进程共享工作区写权限，并使用会话私有临时目录。只在明确允许无沙箱的开发环境时使用 `--sandbox none`。Linux 的 `--resource-mode fixed` 约束 CPU 亲和性以及每个进程的虚拟地址空间。进程树总内存需要宿主 cgroup 配额。

Windows 默认构建文件工具和纯协议测试，可用 `-DDSH_WITH_DEEPSEEK=ON` 启用已安装的 libcurl。Debug/Release 使用 `ctest --test-dir build -C Debug --output-on-failure` 或 `-C Release`。

## DeepSeek 与 MCP

`--model deepseek-flash` 使用 DeepSeek。`--key-file` 读取首个非空行，也可以使用环境变量 `DEEPSEEK_API_KEY` 或数据目录下的 `.env`。`--thinking` 开启思考模式，`--base-url` 和 `--proxy` 支持网关和代理。

`--mcp FILE` 使用形如 `{"servers":[{"name":"demo","command":"server","args":[]}]}` 的本地配置。服务端走 stdio JSON-RPC，其工具以 `mcp__demo__工具名` 注册。

## ARM64

目标 SDK 应提供 AArch64 GNU C++ 编译器、sysroot 中的目标运行库、libcurl 和相应头文件。

```sh
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  --toolchain cmake/aarch64-linux-gnu.cmake \
  -DDSH_TOOLCHAIN_PREFIX=/opt/vehicle-sdk/bin/aarch64-linux-gnu \
  -DDSH_TARGET_SYSROOT=/opt/vehicle-sdk/sysroot
cmake --build build-arm64 --parallel
file build-arm64/dsh-mini
```

目标机验收应执行离线 shell、沙箱与 MCP 检查，并使用 `tests/benchmark.py` 将同机的 Rust Release 和 C++ Release 结果写入对照报告。具体 TodoList、验收条件与原版评测命令见 [PLAN.md](PLAN.md)。
