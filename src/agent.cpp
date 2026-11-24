#include "agent.hpp"

#include "session.hpp"
#include "tools.hpp"

#include <algorithm>
#include <stdexcept>

namespace dsh {

void Operation::check() const {
    if (cancelled && cancelled()) throw std::runtime_error("操作已取消");
    if (Clock::now() >= deadline) throw std::runtime_error("操作超时");
}

std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\n\r\f\v");
    if (begin == text.npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\n\r\f\v") - begin + 1);
}

Json encode_call(const ToolCall& call) {
    return {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}};
}

ToolCall decode_call(const Json& data) {
    return {data.at("id").get<std::string>(), data.at("name").get<std::string>(),
            data.at("arguments")};
}

Completion MockModel::complete(const ModelRequest& request, const DeltaSink& emit) {
    request.operation.check();
    if (const auto* result = std::get_if<ToolResult>(&request.history.back())) {
        std::string reply = "（mock）";
        reply += result->is_error ? "工具返回错误：" : "已收到工具结果：";
        reply += result->name + " [" + result->call_id + "]";
        emit(DeltaKind::text, reply);
        return {{std::move(reply), std::nullopt, {}}};
    }
    const auto* user = std::get_if<UserMessage>(&request.history.back());
    auto input = user ? trim(user->text) : std::string_view{};
    if (input.starts_with("!")) {
        input.remove_prefix(1);
        const auto separator = input.find_first_of(" \t\r\n");
        auto name = input.substr(0, separator);
        if (!name.empty()) {
            auto arguments = separator == input.npos ? std::string_view{} : trim(input.substr(separator));
            auto data = arguments.empty() ? Json::object() : Json::parse(arguments, nullptr, false);
            if (data.is_discarded()) data = nullptr;
            return {{{}, std::nullopt, {{"", std::string(name), std::move(data)}}}};
        }
    }
    if (std::ranges::any_of(request.tools, [](const ToolSchema& tool) { return tool.name == "bash"; })) {
        return {{{}, std::nullopt, {{"", "bash", {{"command", "echo hello from dsh-mini"}}}}}};
    }
    std::string reply = "（mock）" + std::string(input);
    emit(DeltaKind::text, reply);
    return {{std::move(reply), std::nullopt, {}}};
}

Agent::Agent(Session& session, ToolRegistry& tools, Model& model, AgentConfig config)
    : session_(session), tools_(tools), model_(model), config_(std::move(config)) {
    if (config_.max_steps == 0) throw std::invalid_argument("max_steps 必须大于零");
}

TurnOutcome Agent::run_turn(std::string_view input, const DeltaSink& emit) {
    session_.start_turn();
    if (trim(input).empty()) {
        session_.finish_turn(TurnEnd::empty);
        return {0, TurnEnd::empty, {}};
    }
    session_.user_message(input);
    TurnOutcome outcome;
    for (std::size_t step = 1; step <= config_.max_steps; ++step) {
        if (config_.cancelled && config_.cancelled()) {
            outcome.reason = TurnEnd::cancelled;
            break;
        }
        outcome.steps = step;
        session_.step_started(step);
        Completion completion;
        try {
            if (session_.history_over_budget()) {
                throw std::runtime_error("本轮历史超过预算，请增大 --history-budget");
            }
            completion = model_.complete(
                {config_.model, session_.history(), tools_.schemas(),
                 {Clock::now() + std::chrono::minutes(10), config_.cancelled}}, emit);
        } catch (...) {
            session_.step_finished(step, 0);
            if (config_.cancelled && config_.cancelled()) {
                outcome.reason = TurnEnd::cancelled;
                session_.finish_turn(outcome.reason);
                return outcome;
            }
            session_.finish_turn(TurnEnd::failed);
            throw;
        }

        outcome.text = completion.message.text;
        auto& calls = completion.message.calls;
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (calls[index].id.empty()) {
                calls[index].id = "call-" + std::to_string(session_.next_sequence()) +
                                  "-" + std::to_string(index);
            }
        }
        session_.assistant_message(completion);
        for (const auto& call : calls) {
            session_.tool_started(call);
            session_.tool_finished(tools_.execute(
                call, {Clock::now() + config_.tool_timeout, config_.cancelled}));
        }
        session_.step_finished(step, calls.size());

        if (config_.cancelled && config_.cancelled()) {
            outcome.reason = TurnEnd::cancelled;
            break;
        }
        if (calls.empty()) break;
        if (step == config_.max_steps) {
            outcome.reason = TurnEnd::max_steps;
            break;
        }
    }
    session_.finish_turn(outcome.reason);
    return outcome;
}

} // namespace dsh
