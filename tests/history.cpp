#include "agent.hpp"
#include "session.hpp"
#include "tools.hpp"

#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

using namespace dsh;

class Scratch {
public:
    Scratch()
        : path(fs::temp_directory_path() / ("dsh-history-test-" + std::to_string(std::random_device{}()))) {
        if (!fs::create_directory(path)) throw std::runtime_error("cannot create fixture");
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    const fs::path path;
};

void require(bool condition, std::string_view detail) {
    if (!condition) throw std::runtime_error(std::string(detail));
}

class RecordingModel final : public Model {
public:
    Completion complete(const ModelRequest& request, const DeltaSink&) override {
        require(std::get<SystemMessage>(request.history.front()).text == "system", "lost system message");
        require(std::holds_alternative<UserMessage>(request.history.back()), "lost new user input");
        if (calls == 19) {
            require(request.history.size() < 10, "complete history turns were not discarded");
            for (const auto& message : request.history) {
                if (const auto* user = std::get_if<UserMessage>(&message)) {
                    require(!user->text.starts_with("oldest"), "oldest turn leaked into the new request");
                }
            }
        }
        ++calls;
        return {{std::string(200, 'r'), std::nullopt, {}}};
    }

    int calls = 0;
};

void test_history(const fs::path& path) {
    RecordingModel model;
    ToolRegistry tools;
    std::string id;
    std::size_t session_messages = 0;
    {
        Session session(path, path, "mock", "system", std::nullopt, 0, 1024);
        id = session.id();
        Agent agent(session, tools, model, {});
        for (std::size_t index = 0; index < 20; ++index) {
            const auto prefix = index == 0 ? "oldest" : "next";
            require(agent.run_turn(prefix + std::string(200, 'u'), {}).steps == 1, "bad turn");
        }
        session_messages = session.history().size();
    }

    std::ifstream file(path / "sessions" / (id + ".jsonl"), std::ios::binary);
    std::size_t turns = 0;
    for (std::string line; std::getline(file, line);) {
        if (Json::parse(line)["type"] == "turn/end") ++turns;
    }
    require(turns == 20, "history trimming removed persisted events");

    Session resumed(path, path, "mock", "new prompt", id, 0, 1024);
    require(resumed.history().size() == session_messages, "resume projected different history");
    require(std::get<SystemMessage>(resumed.history().front()).text == "system", "resume changed the system message");
}

void test_disk_cap(const fs::path& path) {
    RecordingModel model;
    ToolRegistry tools;
    Session session(path, path, "mock", "system", std::nullopt, 512, 0);
    Agent agent(session, tools, model, {});
    const auto turn = agent.run_turn("ordinary input", {});
    require(turn.reason == TurnEnd::completed && turn.steps == 1, "disk cap interrupted agent");
    const auto log = path / "sessions" / (session.id() + ".jsonl");
    require(fs::file_size(log) <= 512, "disk cap exceeded");
    std::ifstream file(log, std::ios::binary);
    for (std::string line; std::getline(file, line);) {
        require(Json::parse(line).contains("seq"), "disk cap produced a partial JSON line");
    }
}

} // namespace

int main() {
    try {
        Scratch scratch;
        test_history(scratch.path / "history");
        test_disk_cap(scratch.path / "cap");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
