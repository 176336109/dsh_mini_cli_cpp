#pragma once

#ifdef __linux__
#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace dsh {

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }

private:
    int fd_;
};

[[noreturn]] inline void throw_errno(std::string_view action) {
    const int error = errno;
    throw std::system_error(error, std::generic_category(), std::string(action));
}

} // namespace dsh
#endif
