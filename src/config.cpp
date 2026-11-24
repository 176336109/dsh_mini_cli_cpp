#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace dsh {
namespace {

fs::path user_home() {
#ifdef _WIN32
    wchar_t* value = nullptr;
    if (_wdupenv_s(&value, nullptr, L"USERPROFILE") != 0) value = nullptr;
    const std::unique_ptr<wchar_t, decltype(&std::free)> storage(value, std::free);
    return value ? fs::path(value) : fs::path{};
#else
    const char* value = std::getenv("HOME");
    return value ? fs::path(value) : fs::path{};
#endif
}

std::string plain_file(const fs::path& file, std::size_t limit) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("无法读取文件：" + path_to_utf8(file));
    std::string text(limit + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad()) throw std::runtime_error("读取文件失败：" + path_to_utf8(file));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return text;
}

std::string clipped(std::string text, std::size_t max) {
    if (text.size() > max) {
        while ((static_cast<unsigned char>(text[max]) & 0xc0) == 0x80) --max;
        text.resize(max);
        text += "\n…（已截断）";
    }
    return text;
}

std::optional<std::string> dotenv_value(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    if (text.front() == '"' || text.front() == '\'') {
        const auto quote = text.front();
        text.remove_prefix(1);
        std::string result;
        while (!text.empty()) {
            const auto ch = text.front();
            text.remove_prefix(1);
            if (ch == quote) return result;
            if (quote == '"' && ch == '\\' && !text.empty()) {
                const auto escaped = text.front();
                switch (escaped) {
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case '"': case '\\': result.push_back(escaped); break;
                default: result.push_back('\\'); result.push_back(escaped); break;
                }
                text.remove_prefix(1);
            } else {
                result.push_back(ch);
            }
        }
        return std::nullopt;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '#' && (index == 0 || text[index - 1] == ' ' || text[index - 1] == '\t')) {
            text = trim(text.substr(0, index));
            break;
        }
    }
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
}

std::string first_line(std::string_view& text) {
    const auto end = text.find('\n');
    auto line = trim(text.substr(0, end));
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    return std::string(line);
}

struct Skill {
    std::string name;
    std::string description;
};

Skill skill_metadata(std::string_view text, std::string default_name) {
    Skill info{std::move(default_name), ""};
    if (first_line(text) != "---") return info;
    while (!text.empty()) {
        const auto line = first_line(text);
        if (line == "---") break;
        const auto colon = line.find(':');
        if (colon == line.npos) continue;
        const auto name = trim(std::string_view(line).substr(0, colon));
        const auto value = dotenv_value(std::string_view(line).substr(colon + 1));
        if (!value) continue;
        if (name == "name") info.name = clipped(*value, 100);
        if (name == "description") info.description = clipped(*value, 200);
    }
    return info;
}

} // namespace

Environment::Environment(const fs::path& home) {
    const auto global = user_home();
    if (!global.empty()) merge(global / ".dsh-mini" / ".env");
    merge(home / ".env");
}

void Environment::merge(const fs::path& file) {
    if (!fs::is_regular_file(file)) return;
    const auto data = plain_file(file, 64 * 1024);
    if (data.size() > 64 * 1024) throw std::length_error(".env 文件超过上限");
    std::string_view rest(data);
    while (!rest.empty()) {
        auto storage = first_line(rest);
        auto line = std::string_view(storage);
        if (line.starts_with("export ")) line = trim(line.substr(7));
        if (line.empty() || line.starts_with('#')) continue;
        const auto equal = line.find('=');
        if (equal == line.npos) continue;
        const auto name = trim(line.substr(0, equal));
        if (name != "DEEPSEEK_API_KEY" && name != "DEEPSEEK_BASE_URL" && name != "DSH_PROXY") continue;
        if (auto value = dotenv_value(line.substr(equal + 1))) {
            entries_[std::string(name)] = std::move(*value);
        }
    }
}

std::string Environment::get(std::string_view name) const {
#ifdef _MSC_VER
    char* value = nullptr;
    if (_dupenv_s(&value, nullptr, std::string(name).c_str()) != 0) value = nullptr;
    std::unique_ptr<char, decltype(&std::free)> storage(value, std::free);
#else
    const char* value = std::getenv(std::string(name).c_str());
#endif
    if (value && *value) return value;
    const auto item = entries_.find(name);
    return item == entries_.end() ? std::string{} : item->second;
}

std::string read_key(const fs::path& file) {
    const auto data = plain_file(file, 16 * 1024);
    if (data.size() > 16 * 1024) throw std::length_error("Key 文件超过上限");
    std::string_view view(data);
    while (!view.empty()) {
        auto line = first_line(view);
        if (!line.empty()) return line;
    }
    throw std::runtime_error("Key 文件没有非空行");
}

std::string system_prompt(Workspace& workspace, std::span<const ToolSchema> tools,
                          std::string_view model, const fs::path& agents, const fs::path& skills,
                          const fs::path& extra_file, std::string_view extra_prompt) {
    std::string out = "你是 dsh-mini 工作区助手。先核对目标与约束，再用工具取得证据。"
                      "改文件前读相关范围，使用文件工具编辑；改完做最小验证。"
                      "只改用户指定的范围；如实说明结果与未验证项。\n\n"
                      "## 运行环境\n工作目录：";
    out += path_to_utf8(workspace.root());
    out += "\n模型：";
    out += model;
    out += "\n工具：";
    for (const auto& tool : tools) out += " " + tool.name;
    out += '\n';

    auto rules = agents;
    if (rules.empty()) {
        for (const auto name : {"AGENTS.md", "Agents.md"}) {
            const auto candidate = workspace.root() / name;
            if (fs::is_regular_file(candidate)) {
                rules = candidate;
                break;
            }
        }
    }
    if (!rules.empty()) {
        out += "\n## 项目约定\n来源：" + path_to_utf8(rules) + "\n";
        out += clipped(plain_file(rules, 16'000), 16'000);
        out += '\n';
    }

    if (!skills.empty()) {
        const auto root = fs::canonical(skills);
        out += "\n## 技能索引\n按需用 read_file 读取 SKILL.md 正文。\n";
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (paths.size() == 4096) break;
            if (!fs::is_directory(entry.symlink_status())) continue;
            const auto skill = entry.path() / "SKILL.md";
            if (fs::is_regular_file(fs::symlink_status(skill))) paths.push_back(skill);
        }
        std::ranges::sort(paths);
        for (std::size_t index = 0; index < std::min<std::size_t>(32, paths.size()); ++index) {
            const auto& file = paths[index];
            const auto meta = skill_metadata(plain_file(file, 4096), path_to_utf8(file.parent_path().filename()));
            out += "- " + meta.name + " · " + meta.description + " · " + path_to_utf8(file) + '\n';
        }
    }

    if (!extra_file.empty() || !extra_prompt.empty()) {
        out += "\n## 本次附加指令\n";
        out += clipped(extra_file.empty() ? std::string(extra_prompt) : plain_file(extra_file, 16'000), 16'000);
        out += '\n';
    }
    return out;
}

} // namespace dsh
