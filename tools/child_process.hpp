#pragma once

// A child process for the end-to-end tools (link_check, safety_check, soak_check): started, with
// its output into a file if asked, watched (has it exited, how much memory and how many handles
// does it hold), and killed outright when asked or when the object goes. CreateProcess /
// TerminateProcess on Windows, posix_spawn / SIGKILL elsewhere. Header only: it is small and only
// the tools use it.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <psapi.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere else
#endif

namespace msc::tools {

// What a process holds at one moment, for the soak test.
struct ProcessUsage {
    // Memory that is the process's own and not shared: private bytes (Windows), anonymous
    // resident memory (Linux, RssAnon: the heap, not the libraries mapped in). A leak shows here.
    std::uint64_t privateBytes = 0;
    std::uint64_t handles = 0;  // kernel handles (Windows), open file descriptors (Linux)
};

// A child process, killed and reaped when this object goes.
class Child {
public:
    // `output`: a file that receives the child's stdout and stderr (created, or emptied).
    static std::optional<Child> spawn(const std::vector<std::string>& args,
                                      const std::optional<std::string>& output = std::nullopt) {
#if defined(_WIN32)
        std::string commandLine;
        for (const std::string& arg : args) {
            commandLine += '"' + arg + "\" ";
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        HANDLE file = INVALID_HANDLE_VALUE;
        if (output) {
            SECURITY_ATTRIBUTES inherit{};
            inherit.nLength = sizeof(inherit);
            inherit.bInheritHandle = TRUE;
            file = CreateFileA(output->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                return std::nullopt;
            }
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = file;
            startup.hStdError = file;
        }
        PROCESS_INFORMATION info{};
        const BOOL started = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr,
                                            output ? TRUE : FALSE, 0, nullptr, nullptr, &startup, &info);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);  // the child has its own copy
        }
        if (started == 0) {
            return std::nullopt;
        }
        CloseHandle(info.hThread);
        return Child(info.hProcess);
#else
        std::vector<std::string> copies(args);
        std::vector<char*> argv;
        argv.reserve(copies.size() + 1);
        for (std::string& arg : copies) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        if (output) {
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output->c_str(),
                                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
            posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        }
        pid_t pid = 0;
        const int error = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error != 0) {
            return std::nullopt;
        }
        return Child(pid);
#endif
    }

    Child(Child&& other) noexcept
        : process_(std::exchange(other.process_, kNone)),
          exitCode_(std::exchange(other.exitCode_, std::nullopt)) {}
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }

    // The exit code once the process has exited, nothing while it runs. Does not wait. A process
    // killed by a signal reports 128 + the signal, as a shell would.
    std::optional<int> poll() noexcept {
        if (exitCode_ || process_ == kNone) {
            return exitCode_;
        }
#if defined(_WIN32)
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process_, &code);
            exitCode_ = static_cast<int>(code);
        }
#else
        int status = 0;
        if (waitpid(process_, &status, WNOHANG) == process_) {
            exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
#endif
        return exitCode_;
    }

    // Memory and handles now, or nothing if they cannot be read (the process has gone).
    [[nodiscard]] std::optional<ProcessUsage> usage() const {
        if (process_ == kNone || exitCode_) {
            return std::nullopt;
        }
        ProcessUsage usage;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX memory{};
        DWORD handles = 0;
        if (GetProcessMemoryInfo(process_, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)) == 0 ||
            GetProcessHandleCount(process_, &handles) == 0) {
            return std::nullopt;
        }
        usage.privateBytes = memory.PrivateUsage;
        usage.handles = handles;
#else
        const std::string proc = "/proc/" + std::to_string(process_);
        std::ifstream status(proc + "/status");
        if (!status) {
            return std::nullopt;
        }
        for (std::string line; std::getline(status, line);) {
            if (line.starts_with("RssAnon:")) {
                std::istringstream(line.substr(8)) >> usage.privateBytes;  // in kB
                usage.privateBytes *= 1024;
            }
        }
        std::error_code error;
        const std::filesystem::directory_iterator fds(proc + "/fd", error);
        if (error) {
            return std::nullopt;
        }
        usage.handles = static_cast<std::uint64_t>(std::distance(fds, std::filesystem::directory_iterator{}));
#endif
        return usage;
    }

    // Ends the process at once, the way a crash would: no cleanup, no last packet.
    void kill() noexcept {
        if (process_ == kNone) {
            return;
        }
        if (exitCode_) {  // already exited and reaped (poll): the pid may belong to someone else now
#if defined(_WIN32)
            CloseHandle(process_);
#endif
            process_ = kNone;
            return;
        }
#if defined(_WIN32)
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(process_);
#else
        ::kill(process_, SIGKILL);
        waitpid(process_, nullptr, 0);
#endif
        process_ = kNone;
    }

private:
#if defined(_WIN32)
    using Handle = HANDLE;
    static inline const Handle kNone = nullptr;
#else
    using Handle = pid_t;
    static constexpr Handle kNone = 0;
#endif
    explicit Child(Handle process) noexcept : process_(process) {}
    Handle process_;
    std::optional<int> exitCode_;
};

}  // namespace msc::tools
