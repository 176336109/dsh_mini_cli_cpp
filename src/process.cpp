#include "process.hpp"

#ifdef __linux__

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <linux/landlock.h>
#include <poll.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern char** environ;

namespace dsh {
namespace {

constexpr std::uint64_t truncate_access = 1ULL << 14;
constexpr std::uint64_t file_access = LANDLOCK_ACCESS_FS_WRITE_FILE | truncate_access;
constexpr std::uint64_t directory_access = file_access | LANDLOCK_ACCESS_FS_REMOVE_DIR |
    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
    LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
    LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER;

void allow_path(int rules, const fs::path& path, std::uint64_t access) {
    Fd fd(open(path.c_str(), O_PATH | O_CLOEXEC));
    if (!fd) throw_errno("无法打开沙箱可写路径");
    landlock_path_beneath_attr rule{};
    rule.allowed_access = access;
    rule.parent_fd = fd.get();
    if (syscall(SYS_landlock_add_rule, rules, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) < 0) {
        throw_errno("无法设置 Landlock 路径规则");
    }
}

struct Pipe {
    Fd read;
    Fd write;
};

Pipe make_pipe() {
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) < 0) throw_errno("无法建立子进程管道");
    Pipe pipe{Fd(ends[0]), Fd(ends[1])};
    // Keep child stdio setup correct when the parent was launched with a closed standard fd.
    for (auto* end : {&pipe.read, &pipe.write}) {
        if (end->get() < 3) {
            const int moved = fcntl(end->get(), F_DUPFD_CLOEXEC, 3);
            if (moved < 0) throw_errno("无法复制管道描述符");
            end->reset(moved);
        }
    }
    return pipe;
}

void nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw_errno("无法配置非阻塞管道");
    }
}

std::map<std::string, std::string> child_environment(
    const std::map<std::string, std::string>& variables, const fs::path& temporary) {
    std::map<std::string, std::string> result;
    for (char** item = environ; *item; ++item) {
        const std::string_view entry(*item);
        const auto separator = entry.find('=');
        if (separator != entry.npos) {
            result.emplace(entry.substr(0, separator), entry.substr(separator + 1));
        }
    }
    for (const auto& [key, value] : variables) {
        if (key.empty() || key.find_first_of("=\0", 0, 2) != key.npos || value.find('\0') != value.npos) {
            throw std::invalid_argument("无效的子进程环境变量");
        }
        result[key] = value;
    }
    result.erase("DEEPSEEK_API_KEY");
    result.erase("DSH_PROXY");
    if (!temporary.empty()) {
        result["TMPDIR"] = temporary.string();
        result["TMP"] = result["TMPDIR"];
        result["TEMP"] = result["TMPDIR"];
    }
    return result;
}

std::vector<std::string> find_program(std::string_view command, std::string_view path) {
    if (command.find('/') != command.npos) return {std::string(command)};
    std::vector<std::string> candidates;
    while (true) {
        const auto colon = path.find(':');
        const auto directory = path.substr(0, colon);
        candidates.push_back(directory.empty() ? std::string(command) :
                             std::string(directory) + '/' + std::string(command));
        if (colon == path.npos) return candidates;
        path.remove_prefix(colon + 1);
    }
}

[[noreturn]] void child_failure(const char* error, std::size_t length) noexcept {
    (void)::write(STDERR_FILENO, error, length);
    _exit(125);
}

constexpr std::size_t shell_limit = 64 * 1024;

} // namespace

Sandbox::Sandbox(const fs::path& directory, std::string_view mode) {
    if (mode == "none") return;
    if (mode != "landlock" && mode != "auto") throw std::invalid_argument("无效的沙箱模式");
    abi_ = static_cast<int>(syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
    if (abi_ < 3) throw std::runtime_error("Landlock ABI 3 不可用，无法提供所需的文件写入隔离");

    landlock_ruleset_attr options{};
    options.handled_access_fs = directory_access;
    rules_.reset(static_cast<int>(syscall(SYS_landlock_create_ruleset, &options, sizeof(options), 0)));
    if (!rules_) throw_errno("无法创建 Landlock 规则集");

    auto buffer = (fs::temp_directory_path() / "dsh-mini-XXXXXX").string();
    if (!mkdtemp(buffer.data())) throw_errno("无法创建沙箱临时目录");
    temporary_ = buffer;
    try {
        allow_path(rules_.get(), directory, directory_access);
        allow_path(rules_.get(), temporary_, directory_access);
        allow_path(rules_.get(), "/dev/null", file_access);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary_, ignored);
        throw;
    }
}

Sandbox::~Sandbox() {
    if (!temporary_.empty()) {
        std::error_code ignored;
        fs::remove_all(temporary_, ignored);
    }
}

std::string Sandbox::description() const {
    return rules_ ? "landlock ABI " + std::to_string(abi_) : "none";
}

int Sandbox::restrict_child() const noexcept {
    if (!rules_) return 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) return -1;
    return static_cast<int>(syscall(SYS_landlock_restrict_self, rules_.get(), 0));
}

Process::Process(const fs::path& directory, std::span<const std::string> argv,
                 const std::map<std::string, std::string>& variables, const Sandbox& sandbox) {
    if (argv.empty() || argv.front().empty() ||
        std::ranges::any_of(argv, [](const auto& arg) { return arg.find('\0') != arg.npos; })) {
        throw std::invalid_argument("无效的子进程命令");
    }
    auto env = child_environment(variables, sandbox.temporary_directory());
    std::vector<std::string> env_entries;
    for (const auto& [key, value] : env) env_entries.push_back(key + '=' + value);
    std::vector<char*> envp;
    for (auto& entry : env_entries) envp.push_back(entry.data());
    envp.push_back(nullptr);

    auto candidates = find_program(argv.front(), env.contains("PATH") ? env.at("PATH") : "/usr/bin:/bin");
    std::vector<char*> args;
    for (const auto& argument : argv) args.push_back(const_cast<char*>(argument.c_str()));
    args.push_back(nullptr);
    const char* directory_name = directory.c_str();

    auto incoming = make_pipe();
    auto outgoing = make_pipe();
    auto errors = make_pipe();
    nonblocking(incoming.write.get());
    nonblocking(outgoing.read.get());
    nonblocking(errors.read.get());

    struct sigaction default_pipe {};
    default_pipe.sa_handler = SIG_DFL;
    sigemptyset(&default_pipe.sa_mask);

    pid_ = fork();
    if (pid_ < 0) throw_errno("无法启动子进程");
    if (pid_ == 0) {
        if (dup2(errors.write.get(), STDERR_FILENO) < 0) _exit(125);
        constexpr char setup_failed[] = "子进程初始化失败\n";
        if (setpgid(0, 0) < 0 || dup2(incoming.read.get(), STDIN_FILENO) < 0 ||
            dup2(outgoing.write.get(), STDOUT_FILENO) < 0 || chdir(directory_name) < 0 ||
            sigaction(SIGPIPE, &default_pipe, nullptr) < 0 || sandbox.restrict_child() < 0) {
            child_failure(setup_failed, sizeof(setup_failed) - 1);
        }
        for (const auto& candidate : candidates) execve(candidate.c_str(), args.data(), envp.data());
        constexpr char exec_failed[] = "无法运行子进程命令\n";
        child_failure(exec_failed, sizeof(exec_failed) - 1);
    }
    // Close the fork-to-setpgid window for immediate cancellation.
    if (setpgid(pid_, pid_) < 0 && errno != EACCES && errno != ESRCH) {
        const auto error = errno;
        terminate();
        throw std::system_error(error, std::generic_category(), "无法设置子进程组");
    }
    input_ = std::move(incoming.write);
    output_ = std::move(outgoing.read);
    error_ = std::move(errors.read);
}

Process::~Process() { terminate(); }

void Process::terminate() noexcept {
    if (pid_ < 0) return;
    kill(-pid_, SIGKILL);
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
    input_.reset();
    output_.reset();
    error_.reset();
}

bool Process::wait(std::optional<int>& status) {
    if (pid_ < 0) return true;
    int result = 0;
    const auto pid = waitpid(pid_, &result, WNOHANG);
    if (pid < 0 && errno == EINTR) return false;
    if (pid < 0) throw_errno("无法回收子进程");
    if (pid == 0) return false;
    pid_ = -1;
    status = WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
    return true;
}

void Process::drain(Fd& descriptor, std::string& output, std::size_t limit, bool stop_on_limit) {
    if (!descriptor) return;
    std::array<char, 8192> chunk;
    for (int pass = 0; pass < 8; ++pass) {
        const auto count = read(descriptor.get(), chunk.data(), chunk.size());
        if (count == 0) {
            descriptor.reset();
            return;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (count < 0) throw_errno("无法读取子进程管道");
        const auto keep = std::min(static_cast<std::size_t>(count), limit - output.size());
        output.append(chunk.data(), keep);
        if (stop_on_limit && keep != static_cast<std::size_t>(count)) {
            throw std::length_error("子进程输出超出上限");
        }
    }
}

void Process::poll_io(const Operation& operation, bool want_write) {
    operation.check();
    std::array<pollfd, 3> descriptors{{
        {output_.get(), POLLIN, 0}, {error_.get(), POLLIN, 0},
        {want_write ? input_.get() : -1, POLLOUT, 0},
    }};
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        operation.deadline - Clock::now()).count();
    const auto wait_ms = static_cast<int>(std::clamp<decltype(remaining)>(remaining, 0, 50));
    if (poll(descriptors.data(), descriptors.size(), wait_ms) < 0 && errno != EINTR) {
        throw_errno("无法等待子进程管道");
    }
}

void Process::write(std::string_view bytes, const Operation& operation) {
    // The CLI ignores SIGPIPE. A closed stdin is surfaced as an ordinary tool error.
    while (!bytes.empty()) {
        operation.check();
        const auto count = ::write(input_.get(), bytes.data(), std::min<std::size_t>(bytes.size(), 8192));
        if (count > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            drain(output_, output_buffer_, 8 * 1024 * 1024, true);
            drain(error_, errors_, shell_limit, false);
            poll_io(operation, true);
            continue;
        }
        throw_errno("无法写入子进程 stdin");
    }
}

std::string Process::read_line(const Operation& operation, std::size_t limit) {
    while (true) {
        operation.check();
        auto newline = output_buffer_.find('\n');
        if (newline != output_buffer_.npos) {
            if (newline > limit) throw std::length_error("子进程单行输出超出上限");
            auto line = output_buffer_.substr(0, newline);
            output_buffer_.erase(0, newline + 1);
            return line;
        }
        if (output_buffer_.size() > limit) throw std::length_error("子进程单行输出超出上限");
        drain(output_, output_buffer_, limit + 65536, true);
        drain(error_, errors_, shell_limit, false);
        if (!output_ && output_buffer_.find('\n') == output_buffer_.npos) {
            throw std::runtime_error("子进程提前关闭 stdout");
        }
        if (output_buffer_.find('\n') == output_buffer_.npos) poll_io(operation);
    }
}

std::string Process::collect(const Operation& operation) {
    std::optional<int> status;
    std::string truncation;
    try {
        while (true) {
            operation.check();
            drain(output_, output_buffer_, shell_limit, true);
            drain(error_, errors_, shell_limit, true);
            if (!output_ && !error_ && wait(status)) break;
            poll_io(operation);
        }
    } catch (const std::length_error&) {
        terminate();
        truncation = "（子进程输出超出上限，已终止）\n";
    }
    auto text = truncation + "exit_code: " + (status ? std::to_string(*status) : "output_limit");
    if (!output_buffer_.empty()) text += "\nstdout:\n" + output_buffer_;
    if (!errors_.empty()) text += "\nstderr:\n" + errors_;
    return text;
}

void register_shell_tool(ToolRegistry& tools, const fs::path& cwd, const Sandbox& sandbox) {
    tools.add({"bash", "在工作目录执行 shell 命令，返回退出码、stdout 和 stderr。",
               {{"type", "object"}, {"properties", {{"command", {{"type", "string"}}}}},
                {"required", Json::array({"command"})}}},
              [&cwd, &sandbox](const Json& args, const Operation& operation) {
        const std::array<std::string, 3> argv{"/bin/sh", "-c", require_string(args, "command")};
        Process process(cwd, argv, {}, sandbox);
        process.close_input();
        return process.collect(operation);
    });
}

} // namespace dsh
#endif
