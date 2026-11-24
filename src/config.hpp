#pragma once

#include "agent.hpp"
#include "workspace.hpp"

#include <map>

namespace dsh {

class Environment {
public:
    explicit Environment(const fs::path& home);
    [[nodiscard]] std::string get(std::string_view name) const;

private:
    std::map<std::string, std::string, std::less<>> entries_;
    void merge(const fs::path& file);
};

std::string read_key(const fs::path& file);
std::string system_prompt(Workspace& workspace, std::span<const ToolSchema> tools,
                          std::string_view model, const fs::path& agents,
                          const fs::path& skills, const fs::path& extra_file,
                          std::string_view extra_prompt);

} // namespace dsh
