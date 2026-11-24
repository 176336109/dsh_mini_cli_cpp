#include "agent.hpp"
#include "config.hpp"
#include "deepseek.hpp"
#include "mcp.hpp"
#include "process.hpp"
#include "quota.hpp"
#include "session.hpp"
#include "tools.hpp"
#include "workspace.hpp"

#include <algorithm>
#include <charconv>
#include <csignal>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int) { interrupted = 1; }
bool cancelled() noexcept { return interrupted != 0; }

struct Cli {
    dsh::fs::path cwd = ".";
    dsh::fs::path home;
    dsh::fs::path key_file;
    dsh::fs::path agents_file;
    dsh::fs::path skills_dir;
    dsh::fs::path system_file;
    dsh::fs::path mcp;
    std::string model = "mock";
    std::string base_url;
    std::string proxy;
    std::string system_prompt;
    std::string sandbox = "auto";
    std::string resource_mode;
    std::optional<std::string> resume;
    std::optional<std::string> prompt;
    std::optional<std::size_t> cpu_quota;
    std::optional<std::size_t> mem_quota;
    std::size_t max_steps = 24;
    std::size_t timeout = 120;
    std::size_t log_limit = 32;
    std::size_t history_budget = 2;
    bool thinking = false;
    bool help = false;
    bool version = false;
};

std::size_t number(std::string_view text, std::string_view option, std::size_t maximum) {
    std::size_t result = 0;
    const auto [tail, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || tail != text.data() + text.size() || result > maximum) {
        throw std::invalid_argument("无效的数值：" + std::string(option));
    }
    return result;
}

Cli parse(std::span<const std::string> arguments) {
    Cli cli;
    bool positional_only = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string_view argument = arguments[index];
        if (!positional_only && argument == "--") {
            positional_only = true;
        } else if (!positional_only && argument.starts_with("-")) {
            const auto equal = argument.find('=');
            const auto option = argument.substr(0, equal);
            const auto value = [&]() -> std::string_view {
                if (equal != argument.npos) return argument.substr(equal + 1);
                if (index + 1 >= arguments.size() || arguments[index + 1].starts_with("--")) {
                    throw std::invalid_argument("缺少参数值：" + std::string(option));
                }
                return arguments[++index];
            };
            const auto path = [&] { return dsh::path_from_utf8(value()); };
            if (option == "-h" || option == "--help") {
                cli.help = true;
            } else if (option == "-V" || option == "--version") {
                cli.version = true;
            } else if (option == "--cwd") {
                cli.cwd = path();
            } else if (option == "--home") {
                cli.home = path();
            } else if (option == "--model") {
                cli.model = value();
            } else if (option == "--base-url") {
                cli.base_url = value();
            } else if (option == "--proxy") {
                cli.proxy = value();
            } else if (option == "--key-file") {
                cli.key_file = path();
            } else if (option == "--thinking") {
                cli.thinking = true;
            } else if (option == "--agents-file") {
                cli.agents_file = path();
            } else if (option == "--skills-dir") {
                cli.skills_dir = path();
            } else if (option == "--system-file") {
                cli.system_file = path();
            } else if (option == "--system-prompt") {
                cli.system_prompt = value();
            } else if (option == "--mcp") {
                cli.mcp = path();
            } else if (option == "--sandbox") {
                cli.sandbox = value();
            } else if (option == "--resource-mode") {
                cli.resource_mode = value();
            } else if (option == "--cpu-quota") {
                cli.cpu_quota = number(value(), option, 1024);
            } else if (option == "--mem-quota") {
                cli.mem_quota = number(value(), option, 4095);
            } else if (option == "--resume") {
                cli.resume = equal != argument.npos ||
                             (index + 1 < arguments.size() && !arguments[index + 1].starts_with("-"))
                             ? std::string(value()) : "latest";
            } else if (option == "--max-steps") {
                cli.max_steps = std::max<std::size_t>(1, number(value(), option, 1'000'000));
            } else if (option == "--tool-timeout") {
                cli.timeout = number(value(), option, 86'400);
            } else if (option == "--log-max-mb") {
                cli.log_limit = number(value(), option, 4095);
            } else if (option == "--history-budget") {
                cli.history_budget = number(value(), option, 4095);
            } else {
                throw std::invalid_argument("未知参数：" + std::string(option));
            }
        } else {
            if (cli.prompt) *cli.prompt += " ";
            else cli.prompt.emplace();
            *cli.prompt += argument;
        }
    }
    return cli;
}

void print_help() {
    std::cout << "用法：dsh-mini [选项] [提示词...]\n"
              << "  --cwd DIR             工作目录（默认当前目录）\n"
              << "  --home DIR            会话目录（默认工作目录下的 .dsh-mini）\n"
              << "  --model NAME          模型（默认 mock）\n"
              << "  --base-url URL        DeepSeek API 网关\n"
              << "  --proxy URL           HTTP 或 SOCKS 代理\n"
              << "  --key-file FILE       从文件读取密钥\n"
              << "  --thinking            开启 DeepSeek 思考模式\n"
              << "  --agents-file FILE    项目约定文件\n"
              << "  --skills-dir DIR      技能目录\n"
              << "  --system-file FILE    追加系统提示文件\n"
              << "  --system-prompt TEXT  追加系统提示文字\n"
              << "  --mcp FILE            MCP stdio 配置（Linux）\n"
              << "  --sandbox MODE        auto、landlock 或 none\n"
              << "  --resource-mode MODE  free 或 fixed（Linux）\n"
              << "  --cpu-quota N         固定模式 CPU 核数（默认 1）\n"
              << "  --mem-quota MB        固定模式地址空间（默认 64 MiB）\n"
              << "  --resume[=ID]         恢复会话（省略 ID 时恢复最近的会话）\n"
              << "  --max-steps N         单轮步数上限（默认 24）\n"
              << "  --tool-timeout SEC    单次工具超时（默认 120 秒）\n"
              << "  --log-max-mb MB       单个会话日志上限（默认 32 MiB，0 不限）\n"
              << "  --history-budget MB  模型历史预算（默认 2 MiB，0 不限）\n";
}

std::unique_ptr<dsh::Model> build_model(const Cli& cli) {
    if (cli.model == "mock") return std::make_unique<dsh::MockModel>();
    const dsh::Environment env(cli.home);
    dsh::DeepSeekConfig config;
    config.api_key = cli.key_file.empty() ? env.get("DEEPSEEK_API_KEY") : dsh::read_key(cli.key_file);
    if (config.api_key.empty()) throw std::runtime_error("缺少 DeepSeek API Key");
    if (const auto base = env.get("DEEPSEEK_BASE_URL"); !base.empty()) config.base_url = base;
    else if (!cli.base_url.empty()) config.base_url = cli.base_url;
    config.proxy = cli.proxy.empty() ? env.get("DSH_PROXY") : cli.proxy;
    config.thinking = cli.thinking;
    return dsh::make_deepseek(std::move(config));
}

void set_quota(const Cli& cli) {
    const bool given = cli.cpu_quota || cli.mem_quota;
    if (cli.resource_mode == "free" && given) {
        throw std::invalid_argument("free 模式与显式资源配额冲突");
    }
    if (cli.resource_mode == "fixed" || (cli.resource_mode.empty() && given)) {
        auto quota = dsh::apply_quota(cli.cpu_quota.value_or(1), cli.mem_quota.value_or(64));
        std::cerr << "配额 · " << quota << '\n';
    } else if (!cli.resource_mode.empty() && cli.resource_mode != "free") {
        throw std::invalid_argument("无效的资源模式");
    }
}

int run(std::span<const std::string> arguments) {
    try {
        auto cli = parse(arguments);
        if (cli.help) {
            print_help();
            return 0;
        }
        if (cli.version) {
            std::cout << "dsh-mini-cpp 0.1.0\n";
            return 0;
        }
        std::signal(SIGINT, interrupt);
        set_quota(cli);
        dsh::Workspace workspace(cli.cwd);
        if (!cli.skills_dir.empty()) workspace.add_read_root(cli.skills_dir);
        if (cli.home.empty()) cli.home = workspace.root() / ".dsh-mini";
        auto model = build_model(cli);

#ifdef __linux__
        std::signal(SIGPIPE, SIG_IGN);
        dsh::Sandbox sandbox(workspace.root(), cli.sandbox);
        std::cerr << "沙箱 · " << sandbox.description() << '\n';
#else
        if (cli.sandbox != "auto" && cli.sandbox != "none") {
            throw std::invalid_argument("此平台仅支持文件工具，Landlock 需要 Linux");
        }
        if (!cli.mcp.empty()) throw std::invalid_argument("此平台不支持 MCP stdio");
#endif
        dsh::ToolRegistry tools;
        dsh::register_file_tools(tools, workspace);
#ifdef __linux__
        dsh::register_shell_tool(tools, workspace.root(), sandbox);
        if (!cli.mcp.empty()) {
            auto count = dsh::register_mcp_tools(tools, cli.mcp, workspace.root(), sandbox,
                {dsh::Clock::now() + std::chrono::seconds(cli.timeout), cancelled});
            std::cerr << "MCP · 工具=" << count << '\n';
        }
#endif
        constexpr std::size_t mib = 1024 * 1024;
        const auto system = cli.resume ? std::string{} :
            dsh::system_prompt(workspace, tools.schemas(), cli.model, cli.agents_file,
                               cli.skills_dir, cli.system_file, cli.system_prompt);
        dsh::Session session(cli.home, workspace.root(), cli.model, system, cli.resume,
                             cli.log_limit * mib, cli.history_budget * mib);
        dsh::Agent agent(session, tools, *model,
                         {cli.model, cli.max_steps, std::chrono::seconds(cli.timeout), cancelled});
        std::cerr << "dsh-mini · 模型=" << cli.model << " · 会话=" << session.id() << '\n';

        const auto turn = [&](std::string_view input) {
            bool streamed = false;
            const auto outcome = agent.run_turn(input, [&](dsh::DeltaKind kind, std::string_view text) {
                if (kind == dsh::DeltaKind::text) {
                    std::cout << text << std::flush;
                    streamed = true;
                }
            });
            if (!streamed && !outcome.text.empty()) std::cout << outcome.text;
            if (streamed || !outcome.text.empty()) std::cout << '\n';
            return outcome.reason == dsh::TurnEnd::cancelled ? 130 : 0;
        };
        if (cli.prompt) return turn(*cli.prompt);
        for (std::string line; !cancelled() && std::getline(std::cin, line);) {
            if (line == "/exit" || line == "/quit") break;
            if (const int code = turn(line)) return code;
        }
        return cancelled() ? 130 : 0;
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        const auto utf8 = dsh::fs::path(argv[index]).u8string();
        arguments.emplace_back(utf8.begin(), utf8.end());
    }
    return run(arguments);
}
#else
int main(int argc, char* argv[]) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
