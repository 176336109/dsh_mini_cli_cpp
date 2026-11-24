#pragma once

#include "process.hpp"

namespace dsh {

#ifdef __linux__
std::size_t register_mcp_tools(ToolRegistry& tools, const fs::path& config, const fs::path& cwd,
                               const Sandbox& sandbox, const Operation& operation);
#endif

} // namespace dsh
