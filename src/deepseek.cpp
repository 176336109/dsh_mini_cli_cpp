#include "deepseek.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

#ifdef DSH_WITH_DEEPSEEK
#include <curl/curl.h>
#endif

namespace dsh {
namespace {

constexpr std::size_t max_stream_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_frame_bytes = 8 * 1024 * 1024;

Json wire_message(const Message& message, bool thinking) {
    return std::visit([thinking](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, SystemMessage>) {
            return {{"role", "system"}, {"content", item.text}};
        } else if constexpr (std::is_same_v<T, UserMessage>) {
            return {{"role", "user"}, {"content", item.text}};
        } else if constexpr (std::is_same_v<T, ToolResult>) {
            return {{"role", "tool"}, {"tool_call_id", item.call_id}, {"content", item.content}};
        } else {
            Json data = {{"role", "assistant"}, {"content", item.text}};
            if (thinking) data["reasoning_content"] = item.reasoning.value_or("");
            if (!item.calls.empty()) {
                if (item.text.empty()) data["content"] = nullptr;
                auto calls = Json::array();
                for (const auto& call : item.calls) {
                    calls.push_back({{"id", call.id}, {"type", "function"},
                                     {"function", {{"name", call.name},
                                                   {"arguments", call.arguments.dump()}}}});
                }
                data["tool_calls"] = std::move(calls);
            }
            return data;
        }
    }, message);
}

std::string_view optional_string(const Json& data, std::string_view field) {
    const auto value = data.find(field);
    if (value == data.end() || value->is_null()) return {};
    return value->get_ref<const std::string&>();
}

} // namespace

Json deepseek_request(const ModelRequest& request, bool thinking) {
    auto messages = Json::array();
    for (const auto& message : request.history) messages.push_back(wire_message(message, thinking));
    Json body = {{"model", request.model}, {"messages", std::move(messages)}, {"stream", true},
                 {"stream_options", {{"include_usage", true}}},
                 {"thinking", {{"type", thinking ? "enabled" : "disabled"}}}};
    if (!request.tools.empty()) {
        auto tools = Json::array();
        for (const auto& tool : request.tools) {
            tools.push_back({{"type", "function"}, {"function",
                             {{"name", tool.name}, {"description", tool.description},
                              {"parameters", tool.parameters}}}});
        }
        body["tools"] = std::move(tools);
    }
    return body;
}

void DeepSeekStream::feed(std::string_view bytes) {
    // Track CRLF across network chunks. A lone CR is also a valid SSE line terminator.
    for (const char byte : bytes) {
        if (byte == '\n' && previous_cr_) {
            previous_cr_ = false;
            continue;
        }
        previous_cr_ = byte == '\r';
        if (byte == '\r' || byte == '\n') {
            line(line_);
            line_.clear();
        } else {
            if (line_.size() == max_frame_bytes) throw std::length_error("SSE 单行超过上限");
            line_.push_back(byte);
        }
    }
}

void DeepSeekStream::line(std::string_view value) {
    if (done_) return;
    if (value.empty()) {
        dispatch();
        return;
    }
    if (value == "data") value = "data:";
    if (!value.starts_with("data:")) return;
    value.remove_prefix(5);
    if (value.starts_with(" ")) value.remove_prefix(1);
    if (value.size() + 1 > max_frame_bytes - data_.size()) {
        throw std::length_error("SSE 事件超过上限");
    }
    data_.append(value);
    data_.push_back('\n');
}

void DeepSeekStream::dispatch() {
    if (data_.empty()) return;
    data_.pop_back();
    if (trim(data_) == "[DONE]") {
        done_ = true;
    } else {
        consume(Json::parse(data_, nullptr, false));
    }
    data_.clear();
}

void DeepSeekStream::append(std::string& field, std::string_view delta) {
    if (delta.size() > max_stream_bytes - bytes_) throw std::length_error("模型响应超过上限");
    bytes_ += delta.size();
    field.append(delta);
}

void DeepSeekStream::consume(const Json& data) {
    if (!data.is_object() || data.contains("error")) throw std::runtime_error("无效的 DeepSeek SSE 响应");
    const auto usage = data.find("usage");
    if (usage != data.end() && usage->is_object()) {
        usage_ = Usage{usage->value("prompt_tokens", std::size_t{}),
                       usage->value("completion_tokens", std::size_t{})};
    }
    const auto choices = data.find("choices");
    if (choices == data.end() || choices->empty()) return;
    const auto& choice = choices->at(0);
    const auto delta = choice.find("delta");
    if (delta == choice.end() || delta->is_null()) return;
    const auto reasoning = optional_string(*delta, "reasoning_content");
    if (!reasoning.empty()) {
        if (!message_.reasoning) message_.reasoning.emplace();
        append(*message_.reasoning, reasoning);
        if (emit_) emit_(DeltaKind::reasoning, reasoning);
    }
    const auto text = optional_string(*delta, "content");
    if (!text.empty()) {
        append(message_.text, text);
        if (emit_) emit_(DeltaKind::text, text);
    }
    if (const auto pieces = delta->find("tool_calls"); pieces != delta->end() && pieces->is_array()) {
        for (const auto& piece : *pieces) {
            const auto index = piece.at("index").get<std::size_t>();
            if (index >= 64) throw std::length_error("模型工具调用超出数量上限");
            auto& call = calls_[index];
            append(call.id, optional_string(piece, "id"));
            if (const auto function = piece.find("function"); function != piece.end()) {
                append(call.name, optional_string(*function, "name"));
                append(call.arguments, optional_string(*function, "arguments"));
            }
        }
    }
}

Completion DeepSeekStream::finish() {
    if (!done_) throw std::runtime_error("DeepSeek 流在终止事件前断开");
    for (auto& [index, stream] : calls_) {
        (void)index;
        if (stream.id.empty() || stream.name.empty()) {
            throw std::runtime_error("DeepSeek 工具调用缺少 id 或 name");
        }
        if (std::ranges::any_of(message_.calls, [&](const ToolCall& call) { return call.id == stream.id; })) {
            throw std::runtime_error("DeepSeek 工具调用 ID 重复");
        }
        auto args = Json::parse(stream.arguments, nullptr, false);
        if (args.is_discarded()) args = nullptr;
        message_.calls.push_back({std::move(stream.id), std::move(stream.name), std::move(args)});
    }
    return {std::move(message_), usage_};
}

#ifdef DSH_WITH_DEEPSEEK
namespace {

class CurlGlobal {
public:
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("无法初始化 libcurl");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct Transfer {
    DeepSeekStream decoder;
    Operation operation;
    std::exception_ptr failure;
};

std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& transfer = *static_cast<Transfer*>(opaque);
    try {
        transfer.operation.check();
        transfer.decoder.feed({bytes, size * count});
        return size * count;
    } catch (...) {
        transfer.failure = std::current_exception();
        return 0;
    }
}

int check_progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& operation = *static_cast<Operation*>(opaque);
    return Clock::now() >= operation.deadline || (operation.cancelled && operation.cancelled());
}

class Headers {
public:
    ~Headers() { curl_slist_free_all(head); }
    void push(const std::string& value) {
        auto* next = curl_slist_append(head, value.c_str());
        if (!next) throw std::bad_alloc();
        head = next;
    }
    curl_slist* head = nullptr;
};

class DeepSeekModel final : public Model {
public:
    explicit DeepSeekModel(DeepSeekConfig config) : config_(std::move(config)) {
        if (config_.api_key.empty() || config_.api_key.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
            throw std::invalid_argument("无效的 DeepSeek API Key");
        }
        if (!config_.base_url.starts_with("https://") && !config_.base_url.starts_with("http://")) {
            throw std::invalid_argument("DeepSeek 网关必须使用 http 或 https");
        }
        while (config_.base_url.ends_with('/')) config_.base_url.pop_back();
        config_.base_url += "/chat/completions";
        static CurlGlobal runtime;
        curl_.reset(curl_easy_init());
        if (!curl_) throw std::runtime_error("无法创建 libcurl 连接");
    }

    Completion complete(const ModelRequest& request, const DeltaSink& emit) override {
        request.operation.check();
        const auto body = deepseek_request(request, config_.thinking).dump(-1, ' ', false, Json::error_handler_t::replace);
        Transfer transfer{DeepSeekStream(emit), request.operation, {}};
        Headers headers;
        headers.push("Content-Type: application/json");
        headers.push("Accept: text/event-stream");
        headers.push("Authorization: Bearer " + config_.api_key);
        curl_easy_reset(curl_.get());
        const auto set = [this](CURLoption option, auto value) {
            if (curl_easy_setopt(curl_.get(), option, value) != CURLE_OK) {
                throw std::runtime_error("无法配置 libcurl 请求");
            }
        };
        set(CURLOPT_URL, config_.base_url.c_str());
        if (!config_.proxy.empty()) set(CURLOPT_PROXY, config_.proxy.c_str());
        set(CURLOPT_HTTPHEADER, headers.head);
        set(CURLOPT_POSTFIELDS, body.c_str());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_FAILONERROR, 1L);
        set(CURLOPT_CONNECTTIMEOUT, 15L);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            request.operation.deadline - Clock::now()).count();
        set(CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<decltype(remaining)>(1, remaining)));
        set(CURLOPT_WRITEFUNCTION, &receive);
        set(CURLOPT_WRITEDATA, &transfer);
        set(CURLOPT_NOPROGRESS, 0L);
        set(CURLOPT_XFERINFOFUNCTION, &check_progress);
        set(CURLOPT_XFERINFODATA, &transfer.operation);
        auto result = curl_easy_perform(curl_.get());
        if (transfer.failure) std::rethrow_exception(transfer.failure);
        request.operation.check();
        if (result != CURLE_OK) {
            if (result == CURLE_HTTP_RETURNED_ERROR) {
                long status = 0;
                curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &status);
                throw std::runtime_error("DeepSeek HTTP " + std::to_string(status));
            }
            throw std::runtime_error(std::string("DeepSeek 请求失败：") + curl_easy_strerror(result));
        }
        return transfer.decoder.finish();
    }

private:
    DeepSeekConfig config_;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_{nullptr, curl_easy_cleanup};
};

} // namespace

std::unique_ptr<Model> make_deepseek(DeepSeekConfig config) {
    return std::make_unique<DeepSeekModel>(std::move(config));
}
#else
std::unique_ptr<Model> make_deepseek(DeepSeekConfig) {
    throw std::runtime_error("当前构建未链接 libcurl，使用 DSH_WITH_DEEPSEEK 启用 DeepSeek");
}
#endif

} // namespace dsh
