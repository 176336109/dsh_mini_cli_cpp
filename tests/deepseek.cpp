#include "deepseek.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

using namespace dsh;

void require(bool condition, std::string_view label) {
    if (!condition) throw std::runtime_error(std::string(label));
}

void test_stream() {
    std::string streamed;
    const DeltaSink sink = [&](DeltaKind, std::string_view text) { streamed += text; };
    DeepSeekStream decoder(sink);
    const std::string wire =
        ": keep alive\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"先想\",\"tool_calls\":["
        "{\"index\":1,\"id\":\"call_b\",\"function\":{\"name\":\"glob\",\"arguments\":\"{\\\"pat\"}},"
        "{\"index\":0,\"id\":\"call_a\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"pa\"}}"
        "]}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"中\",\"tool_calls\":["
        "{\"index\":0,\"function\":{\"arguments\":\"th\\\":\\\"a.txt\\\"}\"}},"
        "{\"index\":1,\"function\":{\"arguments\":\"tern\\\":\\\"*.txt\\\"}\"}}"
        "]}}]}\n\n"
        "data: {\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":8},\n"
        "data: \"choices\":[]}\n\n"
        "data: [DONE]\n\n";
    for (std::size_t size : {1, 7, 81, 1024}) {
        streamed.clear();
        DeepSeekStream stream(sink);
        for (std::size_t offset = 0; offset < wire.size(); offset += size) {
            stream.feed(std::string_view(wire).substr(offset, size));
        }
        auto response = stream.finish();
        const auto& message = response.message;
        require(message.text == "中" && message.reasoning == "先想", "text and reasoning mismatch");
        require(streamed == "先想中", "deltas mismatch");
        require(message.calls.size() == 2, "call count");
        require(message.calls[0].id == "call_a" && message.calls[0].arguments.at("path") == "a.txt",
                "first call arguments");
        require(message.calls[1].id == "call_b" && message.calls[1].arguments.at("pattern") == "*.txt",
                "second call arguments");
        require(response.usage && response.usage->input == 5 && response.usage->output == 8, "usage");

        const std::array<Message, 4> history{
            SystemMessage{"rules"}, UserMessage{"request"}, response.message,
            ToolResult{"call_a", "read_file", "reply", false}
        };
        const std::array<ToolSchema, 1> tools{{{"read_file", "read", Json::object()}}};
        ModelRequest request{"deepseek-flash", history, tools, {Clock::time_point::max(), nullptr}};
        const auto thinking = deepseek_request(request, true);
        require(thinking["messages"][2]["reasoning_content"] == "先想", "missing reasoning replay");
        require(thinking["messages"][3]["tool_call_id"] == "call_a", "tool result ID");
        const auto ordinary = deepseek_request(request, false);
        require(!ordinary["messages"][2].contains("reasoning_content"), "unexpected reasoning replay");
        require(ordinary["thinking"]["type"] == "disabled", "missing explicit disabled flag");
    }

    decoder.feed("data: {\"choices\":[]}\n\n");
    try {
        (void)decoder.finish();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("disconnected stream was accepted");
}

} // namespace

int main() {
    try {
        test_stream();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
