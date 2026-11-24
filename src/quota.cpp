#include "quota.hpp"

#include <stdexcept>

#ifdef __linux__
#include "linux_fd.hpp"

#include <algorithm>
#include <fstream>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace dsh {

std::string apply_quota(std::size_t cpu_cores, std::size_t memory_mib) {
    if (cpu_cores == 0 || memory_mib == 0) throw std::invalid_argument("固定配额必须大于零");
#ifdef __linux__
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(0, sizeof(current), &current) < 0) throw_errno("无法读取 CPU 亲和性");
    cpu_set_t selected;
    CPU_ZERO(&selected);
    std::size_t used = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && used < cpu_cores; ++cpu) {
        if (CPU_ISSET(cpu, &current)) {
            CPU_SET(cpu, &selected);
            ++used;
        }
    }
    if (used == 0) throw std::runtime_error("当前进程没有可分配的 CPU");

    constexpr rlim_t mib = 1024 * 1024;
    struct rlimit previous {};
    if (getrlimit(RLIMIT_AS, &previous) != 0) throw_errno("无法读取进程地址空间配额");
    auto memory = static_cast<rlim_t>(memory_mib) * mib;
    memory = std::min(memory, std::min(previous.rlim_cur, previous.rlim_max));

    std::ifstream statm("/proc/self/statm");
    rlim_t pages = 0;
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (!(statm >> pages) || page_size <= 0) throw std::runtime_error("无法读取进程地址空间基线");
    const auto baseline = pages * static_cast<rlim_t>(page_size);
    if (memory <= baseline || memory - baseline < 8 * mib) {
        throw std::runtime_error("内存配额无法覆盖当前地址空间和至少 8 MiB 的余量");
    }
    if (sched_setaffinity(0, sizeof(selected), &selected) < 0) throw_errno("无法设置 CPU 亲和性");
    const struct rlimit enforced {memory, memory};
    if (setrlimit(RLIMIT_AS, &enforced) != 0) throw_errno("无法设置进程地址空间配额");

    return "CPU=" + std::to_string(used) + " 核 · 每进程地址空间=" + std::to_string(memory / mib) + " MiB";
#else
    throw std::runtime_error("固定配额需要 Linux");
#endif
}

} // namespace dsh
