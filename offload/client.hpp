#pragma once
#include "protocol.hpp"
#include "embedded.hpp"
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <string>
#include <optional>

extern char** environ;
namespace Offload {
inline bool offloadRequested(pid_t pid);
class Client {
    FD socket;
    uint64_t serial = 0;
    std::vector<std::function<void()>> retainedInputs;
  public:
    pid_t loggerPID = 0, workerPID = 0;
    bool ready = false, stopping = false;
    bool pipelined = true, pipelinedActive = false;
    bool preferLocalInput = true;
    bool sourceDeviceLocal = false;
    uint32_t width = 0, height = 0;
    std::string reason = "idle";
    std::chrono::steady_clock::time_point retryAfter{};
    ~Client() { finish(); }
    bool running() const { return loggerPID > 0; }
    void hold(std::function<void()> input) { retainedInputs.push_back(std::move(input)); }
    void reap() {
        if (!loggerPID) return;
        int status = 0;
        auto r = waitpid(loggerPID, &status, WNOHANG);
        if (r == loggerPID || (r < 0 && errno == ECHILD)) {
            loggerPID = workerPID = 0; ready = stopping = false; socket.reset(); retainedInputs.clear();
            if (reason == "stopping") reason = "idle";
        }
    }
    void stop() {
        socket.reset(); ready = false;
        if (running()) { stopping = true; if (reason == "ready") reason = "stopping"; }
        reap();
    }
    void fail(std::string why) {
        reason = std::move(why);
        retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        stop();
    }
    void finish() {
        stop();
        if (!running()) return;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (running() && std::chrono::steady_clock::now() < end) { reap(); if (running()) usleep(1000); }
        if (running()) kill(loggerPID, SIGTERM);
        end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (running() && std::chrono::steady_clock::now() < end) { reap(); if (running()) usleep(1000); }
        if (running()) {
            if (workerPID > 0) kill(workerPID, SIGKILL);
            // The logger reaps the GPU child before exiting.
            int status = 0; while (waitpid(loggerPID, &status, 0) < 0 && errno == EINTR) {}
            loggerPID = workerPID = 0; retainedInputs.clear();
        }
    }
    void start(uint32_t w, uint32_t h, pid_t candidate) {
        if (!offloadRequested(candidate)) { reason = "render offload flag required"; return; }
        reap();
        if (running() || std::chrono::steady_clock::now() < retryAfter) return;
        int fds[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds)) { reason = "socketpair failed"; return; }
        socket.reset(fds[0]); FD child(fds[1]);
        const char* cacheHome = std::getenv("XDG_CACHE_HOME");
        const char* userHome = std::getenv("HOME");
        if ((!cacheHome || !*cacheHome) && (!userHome || !*userHome)) {
            socket.reset(); reason = "no worker log directory"; return;
        }
        const auto cache = cacheHome && *cacheHome ? std::filesystem::path(cacheHome) : std::filesystem::path(userHome) / ".cache";
        const auto logDirectory = cache / "hyprland-fix-hdr";
        std::error_code logError;
        std::filesystem::create_directories(logDirectory, logError);
        if (logError) { socket.reset(); reason = "cannot create worker log directory"; return; }
        FD log(open((logDirectory / "worker.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
        if (log.value < 0) { socket.reset(); reason = "cannot open worker log"; return; }
        FD executable = createWorkerExecutable();
        if (executable.value < 0) { socket.reset(); reason = "cannot create embedded worker executable"; return; }
        // Keep all source descriptors above the child's fixed IPC/image slots.
        // This also handles starting in a process with low-numbered FD holes.
        for (FD* fd : {&child, &executable, &log}) {
            if (fd->value >= 5) continue;
            const int relocated = fcntl(fd->value, F_DUPFD_CLOEXEC, 5);
            if (relocated < 0) { socket.reset(); reason = "cannot reserve worker descriptors"; return; }
            fd->reset(relocated);
        }
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        int actionError = posix_spawn_file_actions_adddup2(&actions, child.value, 3);
        actionError |= posix_spawn_file_actions_adddup2(&actions, executable.value, 4);
        actionError |= posix_spawn_file_actions_adddup2(&actions, log.value, STDOUT_FILENO);
        actionError |= posix_spawn_file_actions_adddup2(&actions, log.value, STDERR_FILENO);
        actionError |= posix_spawn_file_actions_addclosefrom_np(&actions, 5);
        if (actionError) { posix_spawn_file_actions_destroy(&actions); socket.reset(); reason = "cannot prepare embedded worker spawn"; return; }
        // The logger inherits IPC 3 and executable 4. It closes both after
        // spawning the worker; the worker closes its executable FD after exec.
        std::vector<std::string> environment;
        for (auto entry = environ; *entry; ++entry) {
            std::string e = *entry;
            if (e.starts_with("__NV_PRIME_RENDER_OFFLOAD=") || e.starts_with("__EGL_VENDOR_LIBRARY_FILENAMES=") ||
                e.starts_with("__GLX_VENDOR_LIBRARY_NAME=") || e.starts_with("DRI_PRIME=") ||
                e.starts_with("VK_DRIVER_FILES=") || e.starts_with("VK_ICD_FILENAMES=")) continue;
            environment.push_back(std::move(e));
        }
        environment.emplace_back("__NV_PRIME_RENDER_OFFLOAD=1");
        environment.emplace_back("VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json");
        std::vector<char*> env; for (auto& e : environment) env.push_back(e.data()); env.push_back(nullptr);
        char binary[] = "/proc/self/fd/4", supervise[] = "--supervise";
        char descriptor[] = "3", embeddedFlag[] = "--embedded";
        char* argv[]{binary, supervise, descriptor, embeddedFlag, nullptr};
        auto result = posix_spawn(&loggerPID, binary, &actions, nullptr, argv, env.data());
        posix_spawn_file_actions_destroy(&actions);
        if (result) { loggerPID = 0; socket.reset(); reason = "spawn worker failed"; return; }
        fcntl(socket.value, F_SETFL, fcntl(socket.value, F_GETFL) | O_NONBLOCK);
        Packet hello{}; hello.type = HELLO; hello.width = w; hello.height = h;
        hello.fdFlags = pipelined ? uint32_t(PIPELINED) : 0;
        if (preferLocalInput) hello.fdFlags |= PREFER_LOCAL_INPUT;
        width = w; height = h; ready = stopping = pipelinedActive = sourceDeviceLocal = false; reason = "starting";
        if (!send(socket.value, hello)) fail("cannot initialize worker");
    }
    void checkReady() {
        reap();
        if (!running() || ready || stopping) return;
        Packet response{}; std::vector<FD> fds;
        if (!receive(socket.value, response, fds, MSG_DONTWAIT)) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) fail("worker startup disconnected");
            return;
        }
        if (response.type != READY || !fds.empty()) { fail(response.type == ERROR ? response.error : "invalid worker handshake"); return; }
        workerPID = response.pid; ready = true; pipelinedActive = response.fdFlags & PIPELINED; reason = "ready";
    }
    std::optional<bool> inspectSource(int fd) {
        checkReady(); if (!ready) return std::nullopt;
        Packet query{}; query.type = INSPECT; query.serial = ++serial;
        if (!send(socket.value, query, {fd})) { fail("cannot inspect source"); return std::nullopt; }
        pollfd wait{socket.value, POLLIN, 0};
        // New-buffer metadata queries can contend with swapchain allocation.
        // This is not the per-frame conversion response deadline.
        int result; do { result = poll(&wait, 1, 100); } while (result < 0 && errno == EINTR);
        Packet response; std::vector<FD> descriptors;
        if (result <= 0 || !receive(socket.value, response, descriptors, MSG_DONTWAIT) ||
            response.type != RESULT || response.serial != query.serial || !descriptors.empty()) {
            fail("source inspection failed"); return std::nullopt;
        }
        return bool(response.fdFlags & SOURCE_DEVICE_LOCAL);
    }
    bool convert(Packet job, const std::vector<int>& descriptors, Packet& response, std::vector<FD>& fds, std::function<void()> input) {
        checkReady(); if (!ready) return false;
        job.type = CONVERT; job.serial = ++serial;
        if (!send(socket.value, job, descriptors)) { fail("cannot submit conversion"); return false; }
        pollfd p{socket.value, POLLIN, 0};
        int r; do { r = poll(&p, 1, 20); } while (r < 0 && errno == EINTR);
        if (r <= 0 || !receive(socket.value, response, fds, MSG_DONTWAIT)) {
            hold(std::move(input)); fail("conversion submission timed out"); return false;
        }
        if (response.type != RESULT || response.serial != job.serial || response.slot != job.slot ||
            fds.size() != size_t(1 + bool(response.fdFlags & COMPLETE))) {
            hold(std::move(input)); fail(response.type == ERROR ? response.error : "invalid conversion response"); return false;
        }
        sourceDeviceLocal = response.fdFlags & SOURCE_DEVICE_LOCAL;
        return true;
    }
};
inline bool offloadRequested(pid_t pid) {
    if (pid <= 0) return false;
    std::ifstream stream("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
    if (!stream) return false;
    std::string variable;
    while (std::getline(stream, variable, '\0')) if (variable == "__NV_PRIME_RENDER_OFFLOAD=1") return true;
    return false;
}
}
