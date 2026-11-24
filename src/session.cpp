#include "session.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dsh {

class SessionLock {
public:
    explicit SessionLock(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("无法锁定会话或会话正在使用");
        }
#else
        fd_ = open(path.c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
        if (fd_ == -1 || flock(fd_, LOCK_EX | LOCK_NB) == -1) {
            if (fd_ != -1) close(fd_);
            throw std::runtime_error("无法锁定会话或会话正在使用");
        }
#endif
    }
    ~SessionLock() {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(fd_);
#endif
    }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int fd_;
#endif
};

namespace {

constexpr std::size_t line_bytes_limit = 32 * 1024 * 1024;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string make_session_id() {
    return "s" + std::to_string(now_ms()) + "-" + std::to_string(std::random_device{}());
}

bool valid_id(std::string_view id) {
    constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    return !id.empty() && id.size() <= 128 && id.find_first_not_of(alphabet) == id.npos;
}

std::string latest_session(const fs::path& directory) {
    std::string best;
    fs::file_time_type latest = fs::file_time_type::min();
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!fs::is_regular_file(entry.symlink_status()) || entry.path().extension() != ".jsonl") {
            continue;
        }
        auto id = entry.path().stem().string();
        if (!valid_id(id)) continue;
        const auto mtime = entry.last_write_time();
        if (mtime > latest || (mtime == latest && id > best)) {
            latest = mtime;
            best = std::move(id);
        }
    }
    if (best.empty()) throw std::runtime_error("没有可续跑的会话");
    return best;
}

Json content(std::string_view text) {
    return Json::array({Json{{"kind", "text"}, {"text", text}}});
}

ToolResult decode_result(const Json& data) {
    return {data.at("callId").get<std::string>(), data.at("name").get<std::string>(),
            data.at("content").get<std::string>(), data.at("isError").get<bool>()};
}

AssistantMessage decode_message(const Json& data) {
    AssistantMessage message;
    const auto& blocks = data.at("content");
    if (!blocks.is_array()) throw std::runtime_error("消息必须包含内容数组");
    for (const auto& block : blocks) {
        const auto& kind = block.at("kind").get_ref<const std::string&>();
        const auto& value = block.at("text").get_ref<const std::string&>();
        if (kind == "text") {
            message.text += value;
        } else if (kind == "reasoning") {
            if (!message.reasoning) message.reasoning.emplace();
            *message.reasoning += value;
        } else {
            throw std::runtime_error("未知的会话内容类型");
        }
    }
    if (const auto calls = data.find("toolCalls"); calls != data.end()) {
        if (!calls->is_array()) throw std::runtime_error("toolCalls 必须是数组");
        for (const auto& call : *calls) message.calls.push_back(decode_call(call));
    }
    return message;
}

std::string_view reason_name(TurnEnd reason) {
    switch (reason) {
    case TurnEnd::completed: return "completed";
    case TurnEnd::empty: return "empty";
    case TurnEnd::max_steps: return "max_steps";
    case TurnEnd::cancelled: return "cancelled";
    case TurnEnd::failed: return "failed";
    }
    throw std::logic_error("未知的 turn 结束原因");
}

std::size_t message_bytes(const Message& message) {
    return std::visit([](const auto& item) -> std::size_t {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, ToolResult>) {
            return item.content.size() + item.call_id.size() + item.name.size();
        } else if constexpr (std::is_same_v<T, AssistantMessage>) {
            std::size_t bytes = item.text.size() + (item.reasoning ? item.reasoning->size() : 0);
            for (const auto& call : item.calls) {
                bytes += call.id.size() + call.name.size() + call.arguments.dump().size();
            }
            return bytes;
        } else {
            return item.text.size();
        }
    }, message);
}

} // namespace

Session::Session(const fs::path& home, const fs::path& cwd, const std::string& model,
                 std::string_view prompt, const std::optional<std::string>& resume,
                 std::uintmax_t log_limit, std::size_t history_budget)
    : log_limit_(log_limit), history_budget_(history_budget) {
    auto directory = home / "sessions";
    fs::create_directories(directory);
    id_ = resume ? (*resume == "latest" ? latest_session(directory) : *resume) : make_session_id();
    if (!valid_id(id_)) throw std::runtime_error("无效的会话 ID");
    path_ = directory / (id_ + ".jsonl");
    lock_ = std::make_unique<SessionLock>(directory / (id_ + ".lock"));
    const auto status = fs::symlink_status(path_);
    if (resume) {
        if (!fs::is_regular_file(status)) throw std::runtime_error("找不到会话日志：" + id_);
        load();
    } else if (fs::exists(status)) {
        throw std::runtime_error("会话日志已经存在：" + id_);
    }
    file_.open(path_, std::ios::binary | std::ios::app);
    if (!file_) throw std::runtime_error("无法写入会话日志：" + id_);

    if (!started_) {
        const auto utf8 = cwd.u8string();
        const std::string directory_name(utf8.begin(), utf8.end());
        append("session/start", {{"cwd", directory_name}, {"model", model}});
    }
    if (history_.empty() || !std::holds_alternative<SystemMessage>(history_.front())) {
        append("system/message", {{"content", content(prompt)}}, true);
    }
    recover();
}

Session::~Session() = default;

void Session::load() {
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取会话：" + id_);
    auto* stream = input.rdbuf();
    std::string line;
    std::size_t number = 1;
    for (int ch; (ch = stream->sbumpc()) != std::char_traits<char>::eof();) {
        if (ch != '\n') {
            if (line.size() >= line_bytes_limit) throw std::runtime_error("会话事件超出行上限");
            line.push_back(static_cast<char>(ch));
            continue;
        }
        file_bytes_ += line.size() + 1;
        if (!trim(line).empty()) {
            try {
                auto event = Json::parse(line);
                if (!event.at("seq").is_number_unsigned()) {
                    throw std::runtime_error("会话序号必须是非负整数");
                }
                const auto seq = event.at("seq").get<std::uint64_t>();
                if (seq < next_sequence_ || seq == UINT64_MAX) {
                    throw std::runtime_error("会话序号未严格递增");
                }
                next_sequence_ = seq + 1;
                const auto type = event.at("type").get<std::string>();
                const auto surface = event.value("surfaceOp", std::string("append"));
                if (surface != "append") {
                    if (!event.value("ignorable", false)) throw std::runtime_error("不支持的表面操作");
                } else {
                    try {
                        (void)event.at("time").get<std::int64_t>();
                        apply(type, event.at("data"));
                    } catch (const std::invalid_argument&) {
                        if (!event.value("ignorable", false)) throw;
                    }
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("会话第 " + std::to_string(number) + " 行：" + error.what());
            }
        }
        ++number;
        line.clear();
    }
    input.close();
    if (!line.empty()) {
        if (fs::file_size(path_) != file_bytes_ + line.size()) {
            throw std::runtime_error("会话日志读取未完成");
        }
        fs::resize_file(path_, file_bytes_);
        std::cerr << "警告：已丢弃未写完的会话末行\n";
    } else if (fs::file_size(path_) != file_bytes_) {
        throw std::runtime_error("会话日志读取未完成");
    }
}

void Session::recover() {
    while (!pending_.empty()) {
        const auto& call = pending_.front();
        tool_finished({call.id, call.name, "工具执行中断，结果未知。由用户决定是否重新执行。", true});
    }
    if (turn_open_) finish_turn(TurnEnd::cancelled);
}

void Session::append(std::string_view type, Json data, bool surface) {
    if (file_.is_open()) {
        Json event = {{"type", type}, {"seq", next_sequence_}, {"time", now_ms()},
                      {"data", std::move(data)}};
        if (surface) event["surfaceOp"] = "append";
        auto line = event.dump(-1, ' ', false, Json::error_handler_t::replace);
        line.push_back('\n');
        if (line.size() <= line_bytes_limit &&
            (log_limit_ == 0 || (file_bytes_ <= log_limit_ && line.size() <= log_limit_ - file_bytes_))) {
            file_.write(line.data(), static_cast<std::streamsize>(line.size()));
            file_.flush();
            file_bytes_ += line.size();
        } else {
            file_.setstate(std::ios::failbit);
        }
        if (!file_) {
            std::cerr << "警告：会话日志达到上限或写入失败，已停止落盘\n";
            file_.close();
        }
        data = std::move(event["data"]);
    }
    ++next_sequence_;
    apply(type, data);
}

void Session::apply(std::string_view type, const Json& data) {
    if (type == "session/start") {
        (void)data.at("cwd").get<std::string>();
        (void)data.at("model").get<std::string>();
        started_ = true;
    } else if (type == "turn/start") {
        turn_ = data.at("turn").get<std::uint64_t>();
        turn_open_ = true;
    } else if (type == "turn/end") {
        (void)data.at("turn").get<std::uint64_t>();
        (void)data.at("reason").get<std::string>();
        turn_open_ = false;
        if (history_budget_ && pending_.empty() && history_.size() > 1) {
            const auto boundary = removed_messages_ + history_.size();
            if (completed_turns_.empty() || completed_turns_.back() < boundary) {
                completed_turns_.push_back(boundary);
            }
            enforce_budget();
        }
    } else if (type == "system/message") {
        add_message(SystemMessage{decode_message(data).text});
    } else if (type == "user/message") {
        add_message(UserMessage{decode_message(data).text});
    } else if (type == "assistant/message") {
        auto message = decode_message(data);
        if (!pending_.empty()) throw std::runtime_error("前一次工具请求缺少结果");
        pending_ = message.calls;
        add_message(std::move(message));
    } else if (type == "tool/result") {
        auto result = decode_result(data);
        auto match = std::find_if(pending_.begin(), pending_.end(), [&](const ToolCall& call) {
            return call.id == result.call_id && call.name == result.name;
        });
        if (match == pending_.end()) throw std::runtime_error("工具结果缺少对应调用");
        pending_.erase(match);
        add_message(std::move(result));
    } else if (type == "tool/call") {
        (void)data.at("callId").get<std::string>();
        (void)data.at("name").get<std::string>();
        (void)data.at("arguments");
    } else if (type == "step/start" || type == "step/end") {
        (void)data.at("turn").get<std::uint64_t>();
        (void)data.at("step").get<std::size_t>();
        if (type == "step/end") (void)data.at("toolCalls").get<std::size_t>();
    } else {
        throw std::invalid_argument("未知且不可忽略的事件：" + std::string(type));
    }
}

void Session::add_message(Message message) {
    history_bytes_ += message_bytes(message);
    history_.push_back(std::move(message));
    enforce_budget();
}

void Session::enforce_budget() {
    while (history_budget_ && history_bytes_ > history_budget_ && trim_cursor_ < history_.size()) {
        auto* tool = std::get_if<ToolResult>(&history_[trim_cursor_++]);
        if (!tool || tool->content.size() <= 4096) continue;
        std::size_t keep = 200;
        while ((static_cast<unsigned char>(tool->content[keep]) & 0xc0) == 0x80) --keep;
        const auto old_size = tool->content.size();
        tool->content = "（历史工具输出已省略）\n" + tool->content.substr(0, keep) + "…";
        history_bytes_ -= old_size - tool->content.size();
    }
    while (history_over_budget() && !completed_turns_.empty()) {
        // Keep the initial system message, and evict only complete turns. This preserves
        // the pairing of assistant tool calls with results in the request history.
        const auto count = completed_turns_.front() - removed_messages_ - 1;
        completed_turns_.pop_front();
        auto first = history_.begin() + 1;
        auto end = first + static_cast<std::ptrdiff_t>(count);
        for (auto item = first; item != end; ++item) history_bytes_ -= message_bytes(*item);
        history_.erase(first, end);
        removed_messages_ += count;
        trim_cursor_ = trim_cursor_ > count ? trim_cursor_ - count : 1;
    }
}

std::uint64_t Session::start_turn() {
    append("turn/start", {{"turn", turn_ + 1}});
    return turn_;
}

void Session::step_started(std::size_t step) {
    append("step/start", {{"turn", turn_}, {"step", step}});
}

void Session::step_finished(std::size_t step, std::size_t calls) {
    append("step/end", {{"turn", turn_}, {"step", step}, {"toolCalls", calls}});
}

void Session::user_message(std::string_view input) {
    append("user/message", {{"content", content(input)}}, true);
}

void Session::assistant_message(const Completion& completion) {
    const auto& message = completion.message;
    auto blocks = Json::array();
    if (message.reasoning) blocks.push_back({{"kind", "reasoning"}, {"text", *message.reasoning}});
    if (!message.text.empty()) blocks.push_back({{"kind", "text"}, {"text", message.text}});
    Json data = {{"content", std::move(blocks)}};
    if (!message.calls.empty()) {
        auto calls = Json::array();
        for (const auto& call : message.calls) calls.push_back(encode_call(call));
        data["toolCalls"] = std::move(calls);
    }
    if (completion.usage) {
        data["usage"] = {{"inputTokens", completion.usage->input},
                         {"outputTokens", completion.usage->output}};
    }
    append("assistant/message", std::move(data), true);
}

void Session::tool_started(const ToolCall& call) {
    append("tool/call", {{"callId", call.id}, {"name", call.name}, {"arguments", call.arguments}});
}

void Session::tool_finished(const ToolResult& result) {
    append("tool/result", {{"callId", result.call_id}, {"name", result.name},
                           {"content", result.content}, {"isError", result.is_error}}, true);
}

void Session::finish_turn(TurnEnd reason) {
    append("turn/end", {{"turn", turn_}, {"reason", reason_name(reason)}});
}

} // namespace dsh
