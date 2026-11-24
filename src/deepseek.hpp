#pragma once

#include "agent.hpp"

#include <map>
#include <memory>

namespace dsh {

struct DeepSeekConfig {
    std::string api_key;
    std::string base_url = "https://api.deepseek.com";
    std::string proxy;
    bool thinking = false;
};

Json deepseek_request(const ModelRequest& request, bool thinking);

class DeepSeekStream {
public:
    explicit DeepSeekStream(const DeltaSink& emit) : emit_(emit) {}
    void feed(std::string_view bytes);
    Completion finish();

private:
    struct Call {
        std::string id;
        std::string name;
        std::string arguments;
    };

    void line(std::string_view value);
    void dispatch();
    void consume(const Json& data);
    void append(std::string& field, std::string_view delta);

    const DeltaSink& emit_;
    AssistantMessage message_;
    std::optional<Usage> usage_;
    std::map<std::size_t, Call> calls_;
    std::string line_;
    std::string data_;
    std::size_t bytes_ = 0;
    bool done_ = false;
    bool previous_cr_ = false;
};

std::unique_ptr<Model> make_deepseek(DeepSeekConfig config);

} // namespace dsh
