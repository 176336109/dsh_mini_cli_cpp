#include "tools.hpp"

#include "workspace.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace dsh {
namespace {

std::size_t size_arg(const Json& args, std::string_view key, std::size_t fallback) {
    const auto found = args.find(key);
    if (found == args.end()) return fallback;
    if (found->is_number_unsigned()) return found->get<std::size_t>();
    if (found->is_number_integer() && found->get<std::int64_t>() >= 0) {
        return found->get<std::size_t>();
    }
    throw std::invalid_argument(std::string(key) + " 必须是非负整数");
}

std::string clean_utf8(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index]);
        std::size_t size = 0;
        if (first <= 0x7f) size = 1;
        else if (first >= 0xc2 && first <= 0xdf) size = 2;
        else if (first >= 0xe0 && first <= 0xef) size = 3;
        else if (first >= 0xf0 && first <= 0xf4) size = 4;

        bool valid = size && size <= input.size() - index;
        if (valid && size > 1) {
            const auto next = static_cast<unsigned char>(input[index + 1]);
            valid = !(first == 0xe0 && next < 0xa0) && !(first == 0xed && next >= 0xa0) &&
                    !(first == 0xf0 && next < 0x90) && !(first == 0xf4 && next >= 0x90);
            for (std::size_t part = 1; valid && part < size; ++part) {
                valid = (static_cast<unsigned char>(input[index + part]) & 0xc0) == 0x80;
            }
        }
        if (valid) {
            output.append(input.substr(index, size));
            index += size;
        } else {
            output += "\xef\xbf\xbd";
            ++index;
        }
    }
    return output;
}

std::string limit_output(std::string_view output) {
    constexpr std::size_t limit = 30'000;
    auto end = std::min(output.size(), limit);
    while (end < output.size() && end && (static_cast<unsigned char>(output[end]) & 0xc0) == 0x80) --end;
    auto valid = clean_utf8(output.substr(0, end));
    if (valid.size() > limit) {
        std::size_t cut = limit;
        while ((static_cast<unsigned char>(valid[cut]) & 0xc0) == 0x80) --cut;
        valid.resize(cut);
    }
    if (end < output.size() || valid.size() >= limit) valid += "\n…（输出已截断）";
    return valid;
}

Json fields(std::initializer_list<std::pair<std::string, std::string>> types,
            std::initializer_list<std::string> required) {
    Json properties = Json::object();
    for (const auto& [name, type] : types) properties[name] = {{"type", type}};
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required}};
}

std::string with_crlf(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n' && (i == 0 || value[i - 1] != '\r')) result.push_back('\r');
        result.push_back(value[i]);
    }
    return result;
}

bool matches_glob(std::string_view pattern, std::string_view text) {
    if (pattern.find('/') == pattern.npos) {
        const auto slash = text.rfind('/');
        if (slash != text.npos) text.remove_prefix(slash + 1);
    }
    std::vector<unsigned char> suffix(text.size() + 1);
    std::vector<unsigned char> current(text.size() + 1);
    suffix.back() = true;
    for (auto index = pattern.size(); index > 0;) {
        const auto token = pattern[--index];
        if (token == '/' && index >= 2 && pattern.substr(index - 2, 2) == "**" &&
            (index == 2 || pattern[index - 3] == '/')) {
            index -= 2;
            bool has_directory_match = false;
            current.back() = suffix.back();
            for (std::size_t j = text.size(); j > 0;) {
                --j;
                has_directory_match |= text[j] == '/' && suffix[j + 1];
                current[j] = suffix[j] || has_directory_match;
            }
        } else if (token == '*') {
            const bool deep = index > 0 && pattern[index - 1] == '*';
            if (deep) --index;
            current.back() = suffix.back();
            for (std::size_t j = text.size(); j > 0;) {
                --j;
                current[j] = suffix[j] || ((deep || text[j] != '/') && current[j + 1]);
            }
        } else {
            current.back() = false;
            for (std::size_t j = 0; j < text.size(); ++j) {
                current[j] = (token == text[j] || (token == '?' && text[j] != '/')) && suffix[j + 1];
            }
        }
        suffix.swap(current);
    }
    return suffix.front() != 0;
}

bool search_directory(std::string_view name) {
    if (name == "node_modules" || name == "target" || name == "__pycache__") return false;
    return !name.starts_with('.') || name == ".github" || name == ".claude" || name == ".husky";
}

template <class Visit>
void walk(const Workspace& workspace, const fs::path& directory, const Operation& operation,
          Visit&& visit) {
    struct Item { fs::path path; std::size_t depth; };
    std::vector<Item> pending{{directory, 0}};
    std::size_t budget = 20'000;
    while (budget && !pending.empty()) {
        auto item = std::move(pending.back());
        pending.pop_back();
        const auto entries = workspace.list(path_to_utf8(item.path), budget, operation);
        budget -= entries.size();
        for (const auto& entry : entries) {
            operation.check();
            auto full = item.path / path_from_utf8(entry.name);
            if (entry.is_directory) {
                if (item.depth < 24 && search_directory(entry.name)) {
                    pending.push_back({std::move(full), item.depth + 1});
                }
            } else if (!visit(path_to_utf8(full), path_to_utf8(full.lexically_relative(workspace.root())))) {
                return;
            }
        }
    }
}

std::string ascii_lower(std::string_view text) {
    std::string folded(text);
    for (auto& ch : folded) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return folded;
}

std::string grep(const Workspace& workspace, const Json& args, const Operation& operation) {
    auto pattern = require_string(args, "pattern");
    if (pattern.empty()) throw std::invalid_argument("pattern 不能为空");
    const auto include = args.value("include", std::string{});
    if (include.size() > 512) throw std::invalid_argument("include 过长");
    const bool ignore_case = args.value("ignore_case", false);
    if (ignore_case) pattern = ascii_lower(pattern);
    const auto base = workspace.root() / path_from_utf8(args.value("path", std::string(".")));
    std::string matches;
    std::size_t count = 0;
    walk(workspace, base.lexically_normal(), operation, [&](const std::string& full, const std::string& relative) {
        if (!include.empty() && !matches_glob(include, relative)) return true;
        std::string content;
        try {
            content = workspace.read_text(full, 2 * 1024 * 1024, operation);
        } catch (const std::exception&) {
            operation.check();
            return true;
        }
        if (content.substr(0, 4096).find('\0') != std::string::npos) return true;
        std::string_view rest(content);
        for (std::size_t line = 1; !rest.empty(); ++line) {
            if (line % 128 == 0) operation.check();
            const auto end = rest.find('\n');
            auto segment = rest.substr(0, end);
            if (segment.ends_with('\r')) segment.remove_suffix(1);
            const bool found = ignore_case ? ascii_lower(segment).find(pattern) != std::string::npos
                                           : segment.find(pattern) != segment.npos;
            if (found) {
                matches += relative + ":" + std::to_string(line) + ": ";
                auto end_text = std::min<std::size_t>(segment.size(), 240);
                while (end_text < segment.size() && (static_cast<unsigned char>(segment[end_text]) & 0xc0) == 0x80) {
                    --end_text;
                }
                matches.append(segment.substr(0, end_text));
                matches += '\n';
                if (++count == 200 || matches.size() >= 30'000) return false;
            }
            if (end == rest.npos) break;
            rest.remove_prefix(end + 1);
        }
        return true;
    });
    return count ? matches : "没有匹配";
}

} // namespace

void ToolRegistry::add(ToolSchema schema, ToolHandler handler) {
    if (std::ranges::find(schemas_, schema.name, &ToolSchema::name) != schemas_.end()) {
        throw std::invalid_argument("工具名重复：" + schema.name);
    }
    schemas_.push_back(std::move(schema));
    try {
        handlers_.push_back(std::move(handler));
    } catch (...) {
        schemas_.pop_back();
        throw;
    }
}

ToolResult ToolRegistry::execute(const ToolCall& call, const Operation& operation) const {
    try {
        operation.check();
        const auto match = std::ranges::find(schemas_, call.name, &ToolSchema::name);
        if (match == schemas_.end()) throw std::invalid_argument("未知工具：" + call.name);
        if (!call.arguments.is_object()) throw std::invalid_argument("工具参数必须是 JSON 对象");
        auto output = handlers_[static_cast<std::size_t>(match - schemas_.begin())](call.arguments, operation);
        return {call.id, call.name, limit_output(output), false};
    } catch (const std::exception& error) {
        return {call.id, call.name, limit_output(error.what()), true};
    }
}

std::string require_string(const Json& args, std::string_view key) {
    const auto found = args.find(key);
    if (found == args.end() || !found->is_string()) {
        throw std::invalid_argument("缺少字符串参数：" + std::string(key));
    }
    return found->get<std::string>();
}

void register_file_tools(ToolRegistry& tools, const Workspace& workspace) {
    tools.add({"read_file", "读取带行号的文本。offset 从 1 开始，limit 默认 2000 行。",
               fields({{"path", "string"}, {"offset", "integer"}, {"limit", "integer"}}, {"path"})},
              [&workspace](const Json& args, const Operation& operation) {
        return workspace.read_lines(require_string(args, "path"),
            std::max<std::size_t>(1, size_arg(args, "offset", 1)),
            size_arg(args, "limit", 2000), operation);
    });

    tools.add({"write_file", "写入完整文本，必要时创建父目录。",
               fields({{"path", "string"}, {"content", "string"}}, {"path", "content"})},
              [&workspace](const Json& args, const Operation& operation) {
        const auto path = require_string(args, "path");
        const auto content = require_string(args, "content");
        workspace.write_text(path, content, operation);
        return "已写入 " + std::to_string(content.size()) + " 字节：" + path;
    });

    tools.add({"str_replace", "替换恰好匹配一次的原文，保留文件的换行风格。",
               fields({{"path", "string"}, {"old_string", "string"}, {"new_string", "string"}},
                      {"path", "old_string", "new_string"})},
              [&workspace](const Json& args, const Operation& operation) {
        const auto path = require_string(args, "path");
        auto old = require_string(args, "old_string");
        auto replacement = require_string(args, "new_string");
        if (old.empty()) throw std::invalid_argument("old_string 不能为空");
        auto text = workspace.read_text(path, 8 * 1024 * 1024, operation);
        if (text.find("\r\n") != text.npos && old.find('\r') == old.npos) {
            old = with_crlf(old);
            replacement = with_crlf(replacement);
        }
        const auto first = text.find(old);
        if (first == text.npos) throw std::runtime_error("未找到 old_string");
        if (text.find(old, first + old.size()) != text.npos) throw std::runtime_error("old_string 必须唯一");
        text.replace(first, old.size(), replacement);
        workspace.write_text(path, text, operation);
        return "已替换 1 处：" + path;
    });

    tools.add({"list_dir", "列出目录内容，目录名带 /。默认工作目录。",
               fields({{"path", "string"}}, {})},
              [&workspace](const Json& args, const Operation& operation) {
        std::string output;
        for (const auto& entry : workspace.list(args.value("path", std::string(".")), 500, operation)) {
            output += entry.name;
            if (entry.is_directory) output += '/';
            output += '\n';
        }
        return output.empty() ? std::string("（空目录）") : output;
    });

    tools.add({"grep", "搜索字面字符串，返回路径、行号和匹配行。",
               fields({{"pattern", "string"}, {"path", "string"}, {"include", "string"},
                       {"ignore_case", "boolean"}}, {"pattern"})},
              [&workspace](const Json& args, const Operation& operation) {
        return grep(workspace, args, operation);
    });

    tools.add({"glob", "查找文件。* 匹配单层，** 匹配多层。",
               fields({{"pattern", "string"}, {"path", "string"}}, {"pattern"})},
              [&workspace](const Json& args, const Operation& operation) {
        const auto pattern = require_string(args, "pattern");
        if (pattern.empty() || pattern.size() > 512) throw std::invalid_argument("无效 glob 模式");
        const auto root = workspace.root() / path_from_utf8(args.value("path", std::string(".")));
        std::vector<std::string> paths;
        walk(workspace, root.lexically_normal(), operation,
             [&](const std::string&, const std::string& relative) {
            if (matches_glob(pattern, relative)) paths.push_back(relative);
            return paths.size() < 500;
        });
        std::ranges::sort(paths);
        std::string output;
        for (const auto& path : paths) output += path + '\n';
        return output.empty() ? std::string("没有匹配") : output;
    });
}

} // namespace dsh
