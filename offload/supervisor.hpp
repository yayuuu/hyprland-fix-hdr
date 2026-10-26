#pragma once
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Offload {
inline bool writeAll(int fd, const char* data, size_t size) {
    while (size) {
        const auto written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written; size -= written;
    }
    return true;
}

// This supervisor runs in a freshly spawned copy of the embedded executable.
// It never initializes Vulkan; the GPU child is exec'd before any GPU API use.
inline int superviseWorker(const char* descriptor, const char* embeddedFlag) {
    const auto home = std::getenv("HOME");
    if (!home || !*home) { std::fprintf(stderr, "[hdr-launcher] HOME unavailable\n"); return 1; }
    FD log(open((std::string(home) + "/codex.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
    if (log.value < 0) { std::perror("[hdr-launcher] command log"); return 1; }
    auto tee = [&](const char* data, size_t size) {
        const bool logged = writeAll(log.value, data, size);
        const bool visible = writeAll(STDOUT_FILENO, data, size);
        return logged && visible;
    };
    char timeText[80]{}; const auto now = time(nullptr); tm local{};
    localtime_r(&now, &local); strftime(timeText, sizeof(timeText), "%Y-%m-%dT%H:%M:%S%z", &local);
    char* cwd = getcwd(nullptr, 0);
    const auto header = std::string("\n[") + timeText + "] cwd=" + (cwd ? cwd : "unknown") +
        "\ncommand: /proc/self/fd/4 " + descriptor + " " + embeddedFlag + "\n";
    free(cwd);
    if (!tee(header.data(), header.size())) return 1;

    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC)) { tee("exit_status=1\n", 14); return 1; }
    FD output(pipes[0]), writer(pipes[1]);
    sigset_t blocked{}, previous{};
    sigemptyset(&blocked);
    for (const auto sig : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) sigaddset(&blocked, sig);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) { tee("exit_status=1\n", 14); return 1; }
    FD signals(signalfd(-1, &blocked, SFD_CLOEXEC | SFD_NONBLOCK));
    if (signals.value < 0) { sigprocmask(SIG_SETMASK, &previous, nullptr); tee("exit_status=1\n", 14); return 1; }
    char name[] = "hdr-offload-worker";
    char* arguments[]{name, const_cast<char*>(descriptor), const_cast<char*>(embeddedFlag), nullptr};
    const auto child = fork();
    if (child == 0) {
        if (dup2(writer.value, STDOUT_FILENO) < 0 || dup2(writer.value, STDERR_FILENO) < 0) _exit(126);
        close(output.value); close(writer.value); close(signals.value); close(log.value);
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        fexecve(4, arguments, environ);
        constexpr char error[] = "[hdr-launcher] worker exec failed\n";
        write(STDERR_FILENO, error, sizeof(error) - 1);
        _exit(127);
    }
    if (child < 0) { sigprocmask(SIG_SETMASK, &previous, nullptr); tee("exit_status=1\n", 14); return 1; }
    writer.reset(); close(3); close(4);
    bool eof = false, exited = false, ioFailed = false;
    int status = 0;
    std::chrono::steady_clock::time_point killAfter{};
    while (!exited || !eof) {
        pollfd pending[]{{eof ? -1 : output.value, POLLIN, 0}, {signals.value, POLLIN, 0}};
        const auto result = poll(pending, 2, 100);
        if (result < 0 && errno != EINTR) {
            ioFailed = true; kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            exited = true; break;
        }
        if (pending[1].revents & POLLIN) {
            signalfd_siginfo signal{};
            while (read(signals.value, &signal, sizeof(signal)) == sizeof(signal)) {
                if (signal.ssi_signo == SIGCHLD || exited) continue;
                kill(child, signal.ssi_signo);
                killAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            }
        }
        if (pending[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char data[8192]; const auto size = read(output.value, data, sizeof(data));
            if (size > 0) {
                if (!tee(data, size)) { ioFailed = true; kill(child, SIGKILL); }
            } else if (size == 0 || errno != EINTR) eof = true;
        }
        if (!exited && killAfter != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= killAfter)
            { kill(child, SIGKILL); killAfter = {}; }
        if (!exited) {
            const auto waited = waitpid(child, &status, WNOHANG);
            exited = waited == child;
            if (waited < 0 && errno != EINTR) { ioFailed = true; exited = true; }
        }
    }
    const int code = ioFailed ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    const auto tail = "exit_status=" + std::to_string(code) + "\n";
    tee(tail.data(), tail.size());
    return code;
}
}
