#pragma once

#include "agent.hpp"

#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>

namespace dsh {

class SessionLock;

class Session {
public:
    Session(const fs::path& home, const fs::path& cwd, const std::string& model,
            std::string_view prompt, const std::optional<std::string>& resume,
            std::uintmax_t log_limit, std::size_t history_budget);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] std::span<const Message> history() const { return history_; }
    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] std::uint64_t next_sequence() const { return next_sequence_; }

    [[nodiscard]] bool history_over_budget() const {
        return history_budget_ != 0 && history_bytes_ > history_budget_;
    }

    std::uint64_t start_turn();
    void step_started(std::size_t step);
    void step_finished(std::size_t step, std::size_t calls);
    void user_message(std::string_view input);
    void assistant_message(const Completion& completion);
    void tool_started(const ToolCall& call);
    void tool_finished(const ToolResult& result);
    void finish_turn(TurnEnd reason);

private:
    void load();
    void recover();
    void append(std::string_view type, Json data, bool surface = false);
    void apply(std::string_view type, const Json& data);
    void add_message(Message message);
    void enforce_budget();

    std::string id_;
    fs::path path_;
    std::unique_ptr<SessionLock> lock_;
    std::ofstream file_;
    std::uintmax_t file_bytes_ = 0;
    std::uintmax_t log_limit_;
    std::size_t history_budget_;
    std::size_t history_bytes_ = 0;
    std::size_t trim_cursor_ = 0;
    std::size_t removed_messages_ = 0;
    std::deque<std::size_t> completed_turns_;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t turn_ = 0;
    bool turn_open_ = false;
    bool started_ = false;
    std::vector<ToolCall> pending_;
    std::vector<Message> history_;
};

} // namespace dsh
