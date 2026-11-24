#pragma once

#include <cstddef>
#include <string>

namespace dsh {

std::string apply_quota(std::size_t cpu_cores, std::size_t memory_mib);

} // namespace dsh
