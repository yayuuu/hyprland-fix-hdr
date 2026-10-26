#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <stdexcept>

namespace Offload {
constexpr uint32_t VERSION = 1;
enum Type : uint32_t { HELLO = 1, READY, CONVERT, RESULT, ERROR, INSPECT };
enum PacketFlags : uint32_t { ACQUIRE = 1, REUSE = 2, COMPLETE = 4, PROFILE = 8, PIPELINED = 16, PREFER_LOCAL_INPUT = 32, SOURCE_DEVICE_LOCAL = 64, NATIVE_INPUT = 128 };
struct Packet {
    uint32_t version = VERSION, type = 0, width = 0, height = 0;
    uint32_t format = 0, stride = 0, offset = 0, slot = 0;
    uint64_t modifier = 0, source = 0, serial = 0;
    uint32_t fdFlags = 0, pid = 0;
    // Three padded matrix columns, followed by src min/range, dst min/range.
    std::array<float, 16> color{};
    char error[160]{};
};
class FD {
  public:
    int value = -1;
    FD() = default;
    explicit FD(int fd) : value(fd) {}
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
    FD(FD&& other) noexcept : value(other.release()) {}
    FD& operator=(FD&& other) noexcept { if (this != &other) { reset(); value = other.release(); } return *this; }
    ~FD() { reset(); }
    int release() { return std::exchange(value, -1); }
    void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
};
inline bool send(int sock, const Packet& packet, const std::vector<int>& fds = {}) {
    if (fds.size() > 4) return false;
    iovec io{const_cast<Packet*>(&packet), sizeof(packet)};
    alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))]{};
    msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    if (!fds.empty()) {
        msg.msg_control = control; msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        auto c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(fds.size() * sizeof(int)); std::memcpy(CMSG_DATA(c), fds.data(), fds.size() * sizeof(int));
    }
    return sendmsg(sock, &msg, MSG_NOSIGNAL) == sizeof(packet);
}
inline bool receive(int sock, Packet& packet, std::vector<FD>& fds, int flags = 0) {
    iovec io{&packet, sizeof(packet)};
    alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))]{};
    msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
    auto n = recvmsg(sock, &msg, flags | MSG_CMSG_CLOEXEC);
    if (n <= 0) return false;
    for (auto c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) continue;
        auto bytes = c->cmsg_len - CMSG_LEN(0);
        auto data = reinterpret_cast<int*>(CMSG_DATA(c));
        for (size_t i = 0; i < bytes / sizeof(int); ++i) fds.emplace_back(data[i]);
    }
    return n == sizeof(packet) && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && packet.version == VERSION;
}
}
