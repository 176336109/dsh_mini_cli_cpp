#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace dsh {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments;
};

struct SystemMessage { std::string text; };
struct UserMessage { std::string text; };
struct AssistantMessage {
    std::string text;
    std::optional<std::string> reasoning;
    std::vector<ToolCall> calls;
};
struct ToolResult {
    std::string call_id;
    std::string name;
    std::string content;
    bool is_error = false;
};
using Message = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResult>;

struct Usage {
    std::size_t input = 0;
    std::size_t output = 0;
};

struct Completion {
    AssistantMessage message;
    std::optional<Usage> usage = std::nullopt;
};

struct ToolSchema {
    std::string name;
    std::string description;
    Json parameters;
};

using Clock = std::chrono::steady_clock;
using CancelCheck = bool (*)() noexcept;
struct Operation {
    Clock::time_point deadline;
    CancelCheck cancelled = nullptr;

    void check() const;
};

struct ModelRequest {
    std::string_view model;
    std::span<const Message> history;
    std::span<const ToolSchema> tools;
    Operation operation;
};

enum class DeltaKind { text, reasoning };
using DeltaSink = std::function<void(DeltaKind, std::string_view)>;

class Model {
public:
    virtual ~Model() = default;
    virtual Completion complete(const ModelRequest& request, const DeltaSink& emit) = 0;
};

class MockModel final : public Model {
public:
    Completion complete(const ModelRequest& request, const DeltaSink& emit) override;
};

class Session;
class ToolRegistry;

struct AgentConfig {
    std::string model = "mock";
    std::size_t max_steps = 24;
    std::chrono::seconds tool_timeout{120};
    CancelCheck cancelled = nullptr;
};

enum class TurnEnd { completed, empty, max_steps, cancelled, failed };

struct TurnOutcome {
    std::size_t steps = 0;
    TurnEnd reason = TurnEnd::completed;
    std::string text;
};

class Agent {
public:
    Agent(Session& session, ToolRegistry& tools, Model& model, AgentConfig config);
    TurnOutcome run_turn(std::string_view input, const DeltaSink& emit);

private:
    Session& session_;
    ToolRegistry& tools_;
    Model& model_;
    AgentConfig config_;
};

Json encode_call(const ToolCall& call);
ToolCall decode_call(const Json& data);
std::string_view trim(std::string_view text);

} // namespace dsh
