#pragma once

#include "agent.hpp"
#include "linux_fd.hpp"

#include <cstdio>

namespace dsh {

struct DirectoryEntry {
    std::string name;
    bool is_directory;
};

class Workspace {
public:
    explicit Workspace(const fs::path& root);
    void add_read_root(const fs::path& root);

    [[nodiscard]] const fs::path& root() const { return root_path_; }
    [[nodiscard]] std::string read_lines(std::string_view path, std::size_t offset,
                                        std::size_t limit, const Operation& operation) const;
    [[nodiscard]] std::string read_text(std::string_view path, std::size_t limit,
                                       const Operation& operation) const;
    void write_text(std::string_view path, std::string_view text, const Operation& operation) const;
    [[nodiscard]] std::vector<DirectoryEntry> list(std::string_view path, std::size_t limit,
                                                   const Operation& operation) const;

private:
    struct Root {
        fs::path path;
        fs::path alias;
#ifdef __linux__
        Fd fd;
#endif
    };

    struct Resolved {
        const Root& root;
        fs::path relative;
    };

    [[nodiscard]] static Root open_root(const fs::path& path);
    [[nodiscard]] Resolved resolve(std::string_view path, bool writing = false) const;
    [[nodiscard]] std::FILE* open_text(std::string_view path) const;

    fs::path root_path_;
    std::vector<Root> roots_;
};

fs::path path_from_utf8(std::string_view path);
std::string path_to_utf8(const fs::path& path);

} // namespace dsh
