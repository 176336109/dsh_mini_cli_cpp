#include "workspace.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace dsh {
namespace {

bool within(const fs::path& root, const fs::path& path) {
    auto path_part = path.begin();
    for (const auto& root_part : root) {
        if (path_part == path.end() || root_part != *path_part++) return false;
    }
    return true;
}

#ifdef __linux__
Fd open_directory(int root_fd, const fs::path& relative, bool create = false) {
    Fd directory(openat(root_fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (!directory) throw_errno("无法复制工作目录描述符");
    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        int next = openat(directory.get(), part.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(directory.get(), part.c_str(), 0755) < 0 && errno != EEXIST) {
                throw_errno("无法创建目录");
            }
            next = openat(directory.get(), part.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        }
        if (next < 0) throw_errno("无法打开目录");
        directory.reset(next);
    }
    return directory;
}

std::FILE* open_regular(int root_fd, const fs::path& relative) {
    const auto directory = open_directory(root_fd, relative.parent_path());
    const auto name = relative.filename();
    Fd file(openat(directory.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) throw_errno("无法打开文件");
    struct stat info {};
    if (fstat(file.get(), &info) < 0) throw_errno("无法读取文件属性");
    if (!S_ISREG(info.st_mode)) throw std::invalid_argument("只能读取普通文件");
    auto* stream = fdopen(file.get(), "rb");
    if (!stream) throw_errno("无法创建文件流");
    (void)file.release();
    return stream;
}

void write_regular(int root_fd, const fs::path& relative, std::string_view value,
                   const Operation& operation) {
    const auto directory = open_directory(root_fd, relative.parent_path(), true);
    const auto name = relative.filename();
    struct stat info {};
    mode_t mode = 0644;
    if (fstatat(directory.get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(info.st_mode)) throw std::invalid_argument("只能覆盖普通文件");
        mode = info.st_mode & 0777;
    } else if (errno != ENOENT) {
        throw_errno("无法读取目标文件属性");
    }
    const auto temporary = ".dsh-write-" + std::to_string(std::random_device{}());
    const Fd output(openat(directory.get(), temporary.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (!output) throw_errno("无法创建临时文件");
    try {
        for (auto rest = value; !rest.empty();) {
            operation.check();
            auto written = write(output.get(), rest.data(), std::min<std::size_t>(rest.size(), 65536));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw_errno("无法写入临时文件");
            rest.remove_prefix(static_cast<std::size_t>(written));
        }
        operation.check();
        if (renameat(directory.get(), temporary.c_str(), directory.get(), name.c_str()) < 0) {
            throw_errno("无法提交文件写入");
        }
    } catch (...) {
        unlinkat(directory.get(), temporary.c_str(), 0);
        throw;
    }
}
#endif

} // namespace

fs::path path_from_utf8(std::string_view path) {
    return fs::path(std::u8string(path.begin(), path.end()));
}

std::string path_to_utf8(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

Workspace::Root Workspace::open_root(const fs::path& path) {
    auto alias = fs::absolute(path).lexically_normal();
    if (!alias.has_filename() && alias.has_relative_path()) alias = alias.parent_path();
    auto canonical = fs::canonical(alias);
    if (!fs::is_directory(canonical)) throw std::invalid_argument("工作目录必须是目录");
#ifdef __linux__
    Fd descriptor(open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!descriptor) throw_errno("无法打开工作目录");
    return {std::move(canonical), std::move(alias), std::move(descriptor)};
#else
    return {std::move(canonical), std::move(alias)};
#endif
}

Workspace::Workspace(const fs::path& root) {
    auto opened = open_root(root);
    root_path_ = opened.path;
    roots_.push_back(std::move(opened));
}

void Workspace::add_read_root(const fs::path& root) {
    roots_.push_back(open_root(root));
}

Workspace::Resolved Workspace::resolve(std::string_view input, bool writing) const {
    if (input.empty() || input.find('\0') != input.npos) throw std::invalid_argument("无效文件路径");
    const auto requested = path_from_utf8(input);
    if ((requested.has_root_name() || requested.has_root_directory()) && !requested.is_absolute()) {
        throw std::invalid_argument("无效文件路径");
    }
    auto path = (root_path_ / requested).lexically_normal();
    if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    for (std::size_t index = 0; index < (writing ? 1 : roots_.size()); ++index) {
        const auto& root = roots_[index];
        if (within(root.path, path)) return {root, path.lexically_relative(root.path)};
        if (within(root.alias, path)) return {root, path.lexically_relative(root.alias)};
    }
    throw std::invalid_argument("路径超出工作目录");
}

std::FILE* Workspace::open_text(std::string_view path) const {
    const auto [root, relative] = resolve(path);
#ifdef __linux__
    return open_regular(root.fd.get(), relative);
#else
    const auto resolved = fs::canonical(root.path / relative);
    if (!within(root.path, resolved) || !fs::is_regular_file(resolved)) {
        throw std::invalid_argument("只能读取工作目录内的普通文件");
    }
#ifdef _WIN32
    std::FILE* stream = nullptr;
    if (_wfopen_s(&stream, resolved.c_str(), L"rb") != 0) stream = nullptr;
#else
    auto* stream = std::fopen(resolved.c_str(), "rb");
#endif
    if (!stream) throw std::runtime_error("无法打开文件");
    return stream;
#endif
}

std::string Workspace::read_lines(std::string_view path, std::size_t offset,
                                  std::size_t limit, const Operation& operation) const {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(open_text(path), std::fclose);
    constexpr std::size_t output_limit = 30'000;
    std::string output;
    std::string line;
    std::size_t number = 0;
    std::size_t emitted = 0;
    bool reached_eof = false;
    bool terminated = true;
    while (true) {
        operation.check();
        line.clear();
        ++number;
        int ch = 0;
        std::size_t bytes = 0;
        while ((ch = std::getc(file.get())) != EOF && ch != '\n') {
            if (++bytes % 4096 == 0) operation.check();
            if (number >= offset && emitted < limit && line.size() <= output_limit) {
                line.push_back(static_cast<char>(ch));
            }
        }
        if (ch == EOF && std::ferror(file.get())) throw std::runtime_error("文件读取失败");
        if (ch == EOF && bytes == 0) {
            reached_eof = true;
            break;
        }
        if (number < offset) continue;
        if (emitted == limit) {
            output += "…（还有更多内容，用 offset/limit 继续读）\n";
            break;
        }
        const auto label = std::to_string(number);
        if (label.size() < 6) output.append(6 - label.size(), ' ');
        while (!line.empty() && line.back() == '\r') line.pop_back();
        output += label + "| " + line + "\n";
        ++emitted;
        if (output.size() >= output_limit) break;
        terminated = ch == '\n';
    }
    if (!emitted) {
        output += "（该区间为空）\n";
    } else if (reached_eof) {
        output += terminated ? "（已读到文件末尾，末尾有换行符）\n"
                             : "（已读到文件末尾，末尾没有换行符）\n";
    }
    return output;
}

std::string Workspace::read_text(std::string_view path, std::size_t limit,
                                 const Operation& operation) const {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(open_text(path), std::fclose);
    std::array<char, 8192> buffer;
    std::string result;
    while (true) {
        operation.check();
        const auto count = std::fread(buffer.data(), 1, buffer.size(), file.get());
        if (count > limit - result.size()) throw std::runtime_error("文件内容超出读取上限");
        result.append(buffer.data(), count);
        if (count < buffer.size()) {
            if (std::ferror(file.get())) throw std::runtime_error("文件读取失败");
            return result;
        }
    }
}

void Workspace::write_text(std::string_view path, std::string_view text,
                           const Operation& operation) const {
    const auto [root, relative] = resolve(path, true);
    if (relative == "." || relative.filename().empty()) throw std::invalid_argument("缺少文件名");
#ifdef __linux__
    write_regular(root.fd.get(), relative, text, operation);
#else
    auto current = root.path;
    for (const auto& part : relative.parent_path()) {
        if (part.empty() || part == ".") continue;
        current /= part;
        if (fs::is_symlink(fs::symlink_status(current))) throw std::invalid_argument("目录是符号链接");
        fs::create_directory(current);
        if (!within(root.path, fs::canonical(current))) throw std::invalid_argument("路径超出工作目录");
    }
    const auto target = current / relative.filename();
    const auto status = fs::symlink_status(target);
    if (fs::exists(status) && (!fs::is_regular_file(status) || !within(root.path, fs::canonical(target)))) {
        throw std::invalid_argument("只能覆盖工作目录内的普通文件");
    }
    operation.check();
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) throw std::runtime_error("文件写入失败");
#endif
}

std::vector<DirectoryEntry> Workspace::list(std::string_view path, std::size_t limit,
                                            const Operation& operation) const {
    const auto [root, relative] = resolve(path);
    std::vector<DirectoryEntry> result;
#ifdef __linux__
    auto fd = open_directory(root.fd.get(), relative);
    auto* handle = fdopendir(fd.get());
    if (!handle) throw_errno("无法遍历目录");
    (void)fd.release();
    const auto close_directory = [](DIR* stream) { closedir(stream); };
    std::unique_ptr<DIR, decltype(close_directory)> stream(handle, close_directory);
    while (result.size() < limit) {
        operation.check();
        errno = 0;
        auto* entry = readdir(stream.get());
        if (!entry) {
            if (errno) throw_errno("目录遍历失败");
            break;
        }
        std::string_view name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat info {};
        if (fstatat(dirfd(stream.get()), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) {
            result.push_back({std::string(name), S_ISDIR(info.st_mode)});
        }
    }
#else
    const auto candidate = fs::canonical(root.path / relative);
    if (!within(root.path, candidate)) throw std::invalid_argument("路径超出工作目录");
    for (const auto& entry : fs::directory_iterator(candidate)) {
        operation.check();
        if (result.size() == limit) break;
        const auto status = entry.symlink_status();
        if (fs::is_directory(status) || fs::is_regular_file(status)) {
            result.push_back({path_to_utf8(entry.path().filename()), fs::is_directory(status)});
        }
    }
#endif
    std::ranges::sort(result, {}, &DirectoryEntry::name);
    return result;
}

} // namespace dsh
