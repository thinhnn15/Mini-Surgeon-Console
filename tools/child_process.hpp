#pragma once

// A child process for the end-to-end tools (link_check, safety_check): started, and killed
// outright when asked or when the object goes. CreateProcess / TerminateProcess on Windows,
// posix_spawn / SIGKILL elsewhere. Header only: it is small and only the tools use it.

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
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere else
#endif

namespace msc::tools {

// A child process, killed and reaped when this object goes.
class Child {
public:
    static std::optional<Child> spawn(const std::vector<std::string>& args) {
#if defined(_WIN32)
        std::string commandLine;
        for (const std::string& arg : args) {
            commandLine += '"' + arg + "\" ";
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &info) == 0) {
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
        pid_t pid = 0;
        if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
            return std::nullopt;
        }
        return Child(pid);
#endif
    }

    Child(Child&& other) noexcept : process_(std::exchange(other.process_, kNone)) {}
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }

    // Ends the process at once, the way a crash would: no cleanup, no last packet.
    void kill() noexcept {
        if (process_ == kNone) {
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
};

}  // namespace msc::tools
