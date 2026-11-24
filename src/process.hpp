#pragma once

#include "agent.hpp"
#include "linux_fd.hpp"
#include "tools.hpp"

#include <map>

namespace dsh {

#ifdef __linux__
class Sandbox {
public:
    Sandbox(const fs::path& directory, std::string_view mode);
    ~Sandbox();
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] std::string description() const;
    [[nodiscard]] const fs::path& temporary_directory() const { return temporary_; }
    [[nodiscard]] int restrict_child() const noexcept;

private:
    Fd rules_;
    fs::path temporary_;
    int abi_ = 0;
};

class Process {
public:
    Process(const fs::path& directory, std::span<const std::string> argv,
            const std::map<std::string, std::string>& variables, const Sandbox& sandbox);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    void close_input() noexcept { input_.reset(); }
    void write(std::string_view bytes, const Operation& operation);
    std::string read_line(const Operation& operation, std::size_t limit);
    std::string collect(const Operation& operation);
    void terminate() noexcept;

private:
    void drain(Fd& descriptor, std::string& output, std::size_t limit, bool stop_on_limit);
    bool wait(std::optional<int>& status);
    void poll_io(const Operation& operation, bool want_write = false);

    int pid_ = -1;
    Fd input_;
    Fd output_;
    Fd error_;
    std::string output_buffer_;
    std::string errors_;
};

void register_shell_tool(ToolRegistry& tools, const fs::path& cwd, const Sandbox& sandbox);
#endif

} // namespace dsh
