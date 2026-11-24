#pragma once

#include "agent.hpp"

namespace dsh {

class Workspace;
using ToolHandler = std::function<std::string(const Json&, const Operation&)>;

class ToolRegistry {
public:
    void add(ToolSchema schema, ToolHandler handler);
    [[nodiscard]] std::span<const ToolSchema> schemas() const { return schemas_; }
    [[nodiscard]] ToolResult execute(const ToolCall& call, const Operation& operation) const;

private:
    std::vector<ToolSchema> schemas_;
    std::vector<ToolHandler> handlers_;
};

void register_file_tools(ToolRegistry& tools, const Workspace& workspace);
std::string require_string(const Json& args, std::string_view key);

} // namespace dsh
