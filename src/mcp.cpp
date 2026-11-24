#include "mcp.hpp"
#include "workspace.hpp"

#ifdef __linux__
#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace dsh {
namespace {

constexpr std::string_view version = "2025-06-18";
constexpr std::size_t line_limit = 8 * 1024 * 1024;

void validate_name(std::string_view name) {
    if (name.empty() || name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != name.npos) {
        throw std::invalid_argument("无效的 MCP 工具或服务器名称");
    }
}

class McpClient {
public:
    McpClient(const fs::path& cwd, const std::vector<std::string>& argv,
              const std::map<std::string, std::string>& env, const Sandbox& sandbox)
        : process_(cwd, argv, env, sandbox) {}

    Json initialize(const Operation& operation) {
        auto result = request("initialize",
            {{"protocolVersion", version}, {"capabilities", Json::object()},
             {"clientInfo", {{"name", "dsh-mini"}, {"version", "0.1.0"}}}}, operation);
        if (result.at("protocolVersion").get<std::string>() != version) {
            throw std::runtime_error("MCP 服务端未接受所需的协议版本");
        }
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, operation);
        return request("tools/list", Json::object(), operation).at("tools");
    }

    std::string call(std::string_view name, const Json& args, const Operation& operation) {
        auto result = request("tools/call", {{"name", name}, {"arguments", args}}, operation);
        std::string output;
        const auto content = result.find("content");
        if (content != result.end() && content->is_array()) {
            for (const auto& item : *content) {
                if (item.is_object() && item.value("type", std::string{}) == "text") {
                    if (!output.empty()) output.push_back('\n');
                    output += item.at("text").get_ref<const std::string&>();
                }
            }
        }
        if (output.empty() && result.contains("structuredContent")) output = result.at("structuredContent").dump();
        if (result.value("isError", false)) {
            throw std::runtime_error(output.empty() ? "MCP 工具返回错误" : output);
        }
        return output;
    }

private:
    void send(const Json& message, const Operation& operation) {
        auto frame = message.dump();
        if (frame.size() > line_limit) throw std::length_error("MCP 请求超出行上限");
        frame.push_back('\n');
        process_.write(frame, operation);
    }

    Json request(std::string_view method, Json params, const Operation& operation) {
        if (disconnected_) throw std::runtime_error("MCP 服务连接已关闭");
        const auto id = next_id_++;
        const Operation limit{std::min(operation.deadline, Clock::now() + std::chrono::seconds(60)),
                              operation.cancelled};
        Json reply;
        try {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}, limit);
            while (true) {
                auto message = Json::parse(process_.read_line(limit, line_limit), nullptr, false);
                if (!message.is_object() || message.value("jsonrpc", std::string{}) != "2.0") {
                    throw std::runtime_error("MCP 服务端发送了无效的 JSON-RPC 消息");
                }
                auto message_id = message.find("id");
                if (message.contains("method")) {
                    if (message_id != message.end()) {
                        Json response = {{"jsonrpc", "2.0"}, {"id", *message_id}};
                        if (message.at("method") == "ping") {
                            response["result"] = Json::object();
                        } else {
                            response["error"] = {{"code", -32601}, {"message", "Method not found"}};
                        }
                        send(response, limit);
                    }
                    continue;
                }
                if (message_id != message.end() && *message_id == id) {
                    reply = std::move(message);
                    break;
                }
            }
        } catch (...) {
            disconnected_ = true;
            process_.terminate();
            throw;
        }
        if (reply.contains("error")) {
            throw std::runtime_error("MCP 错误：" + reply.at("error").value("message", std::string{"unknown"}));
        }
        return std::move(reply.at("result"));
    }

    Process process_;
    std::size_t next_id_ = 1;
    bool disconnected_ = false;
};

} // namespace

std::size_t register_mcp_tools(ToolRegistry& tools, const fs::path& config, const fs::path& cwd,
                               const Sandbox& sandbox, const Operation& operation) {
    if (fs::file_size(config) > 1024 * 1024) throw std::length_error("MCP 配置文件过大");
    std::ifstream file(config, std::ios::binary);
    if (!file) throw std::runtime_error("无法读取 MCP 配置");
    const auto settings = Json::parse(file);
    const auto& servers = settings.at("servers");
    if (!servers.is_array() || servers.size() > 16) {
        throw std::invalid_argument("MCP 配置必须包含至多 16 个服务器");
    }
    std::size_t count = 0;
    for (const auto& server : servers) {
        const auto name = require_string(server, "name");
        validate_name(name);
        std::vector<std::string> argv{require_string(server, "command")};
        auto args = server.value("args", std::vector<std::string>{});
        argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
        const auto env = server.value("env", std::map<std::string, std::string>{});
        const auto base = server.contains("cwd") ? cwd / path_from_utf8(require_string(server, "cwd")) : cwd;
        auto client = std::make_shared<McpClient>(base, argv, env, sandbox);
        const auto remote_tools = client->initialize(operation);
        if (!remote_tools.is_array() || remote_tools.size() > 128) {
            throw std::runtime_error("MCP 服务端的工具列表超出上限");
        }
        for (const auto& tool : remote_tools) {
            auto original = require_string(tool, "name");
            validate_name(original);
            const auto full_name = "mcp__" + name + "__" + original;
            if (full_name.size() > 64) throw std::length_error("MCP 工具名超出模型工具名上限");
            auto schema = tool.at("inputSchema");
            if (!schema.is_object()) throw std::invalid_argument("无效的 MCP 工具 schema");
            tools.add({full_name, tool.value("description", std::string{}), std::move(schema)},
                      [client, original = std::move(original)](const Json& input, const Operation& call) {
                return client->call(original, input, call);
            });
            ++count;
        }
    }
    return count;
}

} // namespace dsh
#endif
