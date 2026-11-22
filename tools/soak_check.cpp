// soak_check: a long session, end to end (M6.2, REQ-SYS-001;
// ctest soak.REQ_SYS_001_RepeatedFaultsWithoutLeaksOrDrift).
//
//   soak_check --arm-exe=PATH --console-exe=PATH [--duration=SECONDS] [--interval=SECONDS] [--out=DIR]
//
// Starts arm_sim and console_app --soak as real processes on a free port, and for the whole run
// measures from outside, every interval, the private memory and the handles (file descriptors on
// Linux) of both, and reads what they print: the console's fault cycle, video latency and frame
// rates; the arm's tick lateness and overruns. At the end it fails if:
//   - either process died, or the console's fault cycle was not clean (a fault nobody injected, a
//     fault not detected or the wrong one, a reset accepted too early, stuck), or not every fault
//     was injected at least once;
//   - memory or handles of either process were still growing at the end (msc::soak::judgeGrowth);
//   - the video's p99 latency or the arm's p99 tick lateness got worse over the run (judgeDrift).
// Prints a table of the samples and the verdicts (for the journal). Exits 0 on PASS, 1 on FAIL,
// 77 if the console says this machine cannot run it (it draws on the CPU).

#include "child_process.hpp"
#include "msc/comms/udp_socket.hpp"
#include "msc/soak/fault_cycle.hpp"
#include "msc/soak/trend.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;
using msc::soak::Point;
using msc::tools::Child;

constexpr int kSkipExitCode = 77;
constexpr int kInvalidExitCode = 3;  // console_app --soak: the window was not shown
constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kWarmUp = 30.0;  // seconds: start-up allocations, caches filling, first faults

// The limits. A leak of one video frame (3.7 MB) per minute must fail even the 2-minute run, whose
// judged half is about 45 s: 2.8 MB, so the console's noise floor is 2 MiB. Measured noise of its
// private memory on the dev PC: 0.1 MiB over a run (journal, M6.2).
const msc::soak::GrowthLimit kConsoleMemory{.warmUp = kWarmUp, .maxPerHour = 32 * kMiB, .noise = 2 * kMiB};
const msc::soak::GrowthLimit kArmMemory{.warmUp = kWarmUp, .maxPerHour = 4 * kMiB, .noise = 1 * kMiB};
const msc::soak::GrowthLimit kConsoleHandles{.warmUp = kWarmUp, .maxPerHour = 30, .noise = 10};
const msc::soak::GrowthLimit kArmHandles{.warmUp = kWarmUp, .maxPerHour = 10, .noise = 3};
// Video p99: worse by a third of a frame at 60 Hz and by 15 %. The medians of a clean run moved by
// 0.6 ms (1.8 %) on the dev PC; x1.5 was tried first and let a render thread that had become 6 ms
// slower through (journal, M6.2).
const msc::soak::DriftLimit kVideoLatency{.warmUp = kWarmUp, .maxIncrease = 5.0, .maxRatio = 1.15};
const msc::soak::DriftLimit kArmLateness{.warmUp = kWarmUp, .maxIncrease = 0.5, .maxRatio = 2.0};

using Fields = std::map<std::string, std::string, std::less<>>;

// "head t=1.0 a=2 b=x" -> {t: 1.0, a: 2, b: x}
Fields fieldsOf(std::string_view line) {
    Fields fields;
    std::istringstream in{std::string(line)};
    for (std::string token; in >> token;) {
        if (const auto eq = token.find('='); eq != std::string::npos) {
            fields[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
    return fields;
}

double number(const Fields& fields, std::string_view key) {
    const auto it = fields.find(key);
    if (it == fields.end()) {
        return 0.0;
    }
    try {
        return std::stod(it->second);
    } catch (const std::exception&) {
        return 0.0;
    }
}

// The lines a child has written since the last call (complete lines only).
class LogTail {
public:
    explicit LogTail(fs::path path) : path_(std::move(path)) {}

    std::vector<std::string> newLines() {
        std::ifstream in(path_, std::ios::binary);
        std::vector<std::string> lines;
        if (!in) {
            return lines;
        }
        in.seekg(static_cast<std::streamoff>(offset_));
        const std::string chunk{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::size_t start = 0;
        for (std::size_t end = chunk.find('\n'); end != std::string::npos; end = chunk.find('\n', start)) {
            std::string line = chunk.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
            start = end + 1;
        }
        offset_ += start;
        return lines;
    }

private:
    fs::path path_;
    std::size_t offset_ = 0;
};

struct Series {
    std::vector<Point> consoleMemory, consoleHandles, armMemory, armHandles, videoP99, armLateP99;
};

struct Options {
    std::string armExe;
    std::string consoleExe;
    int duration = 120;
    int interval = 5;
    fs::path out;
};

std::optional<std::string_view> valueOf(std::string_view arg, std::string_view name) {
    if (arg.size() > name.size() + 1 && arg.starts_with(name) && arg[name.size()] == '=') {
        return arg.substr(name.size() + 1);
    }
    return std::nullopt;
}

class SoakCheck {
public:
    explicit SoakCheck(Options options) : options_(std::move(options)) {}

    int run() {
        fs::create_directories(options_.out);
        const std::uint16_t port = [] {
            const auto probe = msc::comms::UdpSocket::open(msc::comms::Endpoint::loopback(0));
            return probe ? probe->localEndpoint().port : std::uint16_t{0};
        }();
        if (port == 0) {
            return fail("cannot find a free port");
        }
        const auto endpoint = msc::comms::Endpoint::loopback(port).toString();
        const auto armLog = options_.out / "arm_sim.log";
        const auto consoleLog = options_.out / "console_app.log";
        auto arm = Child::spawn(
            {options_.armExe, "--listen=" + endpoint, std::format("--report-every={}", options_.interval)},
            armLog.string());
        if (!arm) {
            return fail("cannot start " + options_.armExe);
        }
        std::this_thread::sleep_for(300ms);  // let it bind before the console sends
        auto console = Child::spawn(
            {options_.consoleExe, "--arm=" + endpoint, std::format("--soak={}", options_.duration),
             std::format("--soak-interval={}", options_.interval)},
            consoleLog.string());
        if (!console) {
            return fail("cannot start " + options_.consoleExe);
        }
        std::cout << std::format("soak_check: {} s, a sample every {} s; logs in {}\n", options_.duration,
                                 options_.interval, options_.out.generic_string())
                  << std::format("{:>7} {:>12} {:>8} {:>9} {:>8} {:>8} {:>10} {:>9} {:>11}\n", "t (s)",
                                 "console MiB", "handles", "arm MiB", "handles", "p99 ms", "arm p99 ms",
                                 "faults", "unexpected")
                  << std::flush;

        LogTail armTail(armLog);
        LogTail consoleTail(consoleLog);
        const auto start = Steady::now();
        auto next = start + std::chrono::seconds(options_.interval);
        const auto giveUp = start + std::chrono::seconds(options_.duration + 90);
        std::optional<int> consoleExit;
        while (!consoleExit) {
            std::this_thread::sleep_until(std::min(next, Steady::now() + 200ms));
            readLines(consoleTail, armTail);
            consoleExit = console->poll();
            if (const std::optional<int> armExit = arm->poll()) {
                problems_.push_back(std::format("arm_sim exited during the run (code {})", *armExit));
                break;
            }
            if (Steady::now() >= next) {
                next += std::chrono::seconds(options_.interval);
                sample(std::chrono::duration<double>(Steady::now() - start).count(), *console, *arm);
            }
            if (Steady::now() > giveUp) {
                problems_.emplace_back("the console did not finish in time");
                break;
            }
        }
        readLines(consoleTail, armTail);
        arm->kill();
        console->kill();

        if (consoleExit == kSkipExitCode) {
            std::cout << "soak_check: SKIPPED (the console cannot run the soak test on this machine)\n";
            return kSkipExitCode;
        }
        if (consoleExit == kInvalidExitCode) {
            problems_.emplace_back(
                "invalid run: the console window stopped being shown (see the console's INVALID line); "
                "nothing was measured after that");
        } else if (consoleExit && *consoleExit != 0 && *consoleExit != 1) {
            problems_.push_back(std::format("console_app exited with code {}", *consoleExit));
        }
        judge();
        for (const auto& p : problems_) {
            std::cout << "FAIL: " << p << "\n";
        }
        std::cout << (problems_.empty() ? "PASS\n" : std::format("{} problem(s)\n", problems_.size()));
        return problems_.empty() ? 0 : 1;
    }

private:
    static int fail(const std::string& why) {
        std::cout << "FAIL: " << why << "\n";
        return 1;
    }

    void readLines(LogTail& consoleTail, LogTail& armTail) {
        for (const auto& line : consoleTail.newLines()) {
            if (line.starts_with("soak event") || line.starts_with("soak:")) {
                std::cout << "  console: " << line << "\n" << std::flush;
            }
            if (line.starts_with("soak t=") || line.starts_with("soak done")) {
                const Fields f = fieldsOf(line);
                lastConsole_ = f;
                series_.videoP99.push_back(Point{.t = number(f, "t"), .value = number(f, "p99_ms")});
                done_ = done_ || line.starts_with("soak done");
            }
        }
        for (const auto& line : armTail.newLines()) {
            if (line.starts_with("arm_sim report")) {
                const Fields f = fieldsOf(line);
                lastArm_ = f;
                series_.armLateP99.push_back(Point{.t = number(f, "t"), .value = number(f, "late_p99_ms")});
            } else if (line.find("console link lost") != std::string::npos) {
                ++armLinkLost_;
            }
        }
    }

    void sample(double t, const Child& console, const Child& arm) {
        const auto c = console.usage();
        const auto a = arm.usage();
        if (c) {
            series_.consoleMemory.push_back({t, static_cast<double>(c->privateBytes)});
            series_.consoleHandles.push_back({t, static_cast<double>(c->handles)});
        }
        if (a) {
            series_.armMemory.push_back({t, static_cast<double>(a->privateBytes)});
            series_.armHandles.push_back({t, static_cast<double>(a->handles)});
        }
        std::cout << std::format(
                         "{:>7.0f} {:>12.1f} {:>8} {:>9.2f} {:>8} {:>8.2f} {:>10.3f} {:>9.0f} {:>11.0f}\n", t,
                         c ? static_cast<double>(c->privateBytes) / kMiB : 0.0, c ? c->handles : 0,
                         a ? static_cast<double>(a->privateBytes) / kMiB : 0.0, a ? a->handles : 0,
                         number(lastConsole_, "p99_ms"), number(lastArm_, "late_p99_ms"),
                         number(lastConsole_, "injected"), number(lastConsole_, "unexpected"))
                  << std::flush;
    }

    void growth(std::string_view what, std::span<const Point> points, const msc::soak::GrowthLimit& limit,
                double unit, std::string_view unitName) {
        const auto v = msc::soak::judgeGrowth(points, limit);
        std::cout << std::format("  {:<22} {:+.3g} {}/h over {:.0f}-{:.0f} s ({} samples){}\n", what,
                                 v.perHour / unit, unitName, v.from, v.to, v.points,
                                 v.why.empty() ? "" : ": " + v.why);
        if (!v.ok) {
            problems_.push_back(std::format("{}: {}", what, v.why));
        }
    }

    void drift(std::string_view what, std::span<const Point> points, const msc::soak::DriftLimit& limit) {
        const auto v = msc::soak::judgeDrift(points, limit);
        std::cout << std::format("  {:<22} median {:.3f} first third, {:.3f} last third ({} samples){}\n",
                                 what, v.start, v.end, v.points, v.why.empty() ? "" : ": " + v.why);
        if (!v.ok) {
            problems_.push_back(std::format("{}: {}", what, v.why));
        }
    }

    void judge() {
        std::cout << "verdicts:\n";
        growth("console memory", series_.consoleMemory, kConsoleMemory, kMiB, "MiB");
        growth("console handles", series_.consoleHandles, kConsoleHandles, 1.0, "");
        growth("arm_sim memory", series_.armMemory, kArmMemory, kMiB, "MiB");
        growth("arm_sim handles", series_.armHandles, kArmHandles, 1.0, "");
        drift("video p99 (ms)", series_.videoP99, kVideoLatency);
        drift("arm tick p99 (ms)", series_.armLateP99, kArmLateness);

        if (!done_) {
            problems_.emplace_back("the console never printed \"soak done\"");
            return;
        }
        const auto& f = lastConsole_;
        const double injected = number(f, "injected");
        std::cout << std::format(
            "  fault cycle            {:.0f} injected, {:.0f} detected, {:.0f} recovered; unexpected {:.0f}, "
            "missed {:.0f}, "
            "wrong {:.0f}, early reset {:.0f}, stuck {:.0f}\n",
            injected, number(f, "detected"), number(f, "recovered"), number(f, "unexpected"),
            number(f, "missed"), number(f, "wrong"), number(f, "early_reset"), number(f, "stuck"));
        std::cout << std::format(
            "  arm_sim                {:.0f} overruns, {:.0f} holds, {} \"console link lost\"\n",
            number(lastArm_, "overruns"), number(lastArm_, "holds"), armLinkLost_);
        for (const auto* key : {"unexpected", "missed", "wrong", "early_reset", "stuck"}) {
            if (number(f, key) > 0) {
                problems_.push_back(std::format("fault cycle: {} = {:.0f}", key, number(f, key)));
            }
        }
        if (injected < static_cast<double>(msc::soak::kInjections.size())) {
            problems_.push_back(
                std::format("only {:.0f} faults injected: every one of the {} must be, at least once",
                            injected, msc::soak::kInjections.size()));
        }
    }

    Options options_;
    Series series_;
    Fields lastConsole_;
    Fields lastArm_;
    bool done_ = false;
    int armLinkLost_ = 0;
    std::vector<std::string> problems_;
};

int run(std::span<char*> args) {
    Options options;
    for (const std::string_view arg : args.subspan(1)) {
        if (const auto v = valueOf(arg, "--arm-exe")) {
            options.armExe = *v;
        } else if (const auto c = valueOf(arg, "--console-exe")) {
            options.consoleExe = *c;
        } else if (const auto d = valueOf(arg, "--duration")) {
            options.duration = std::stoi(std::string(*d));
        } else if (const auto i = valueOf(arg, "--interval")) {
            options.interval = std::stoi(std::string(*i));
        } else if (const auto o = valueOf(arg, "--out")) {
            options.out = fs::path(std::string(*o));
        } else {
            std::cerr << "soak_check: unknown option " << arg << "\n";
            return 2;
        }
    }
    if (options.armExe.empty() || options.consoleExe.empty() || options.duration <= 0 ||
        options.interval <= 0) {
        std::cerr << "usage: soak_check --arm-exe=PATH --console-exe=PATH [--duration=SECONDS] "
                     "[--interval=SECONDS] "
                     "[--out=DIR]\n";
        return 2;
    }
    if (options.out.empty()) {
        options.out = fs::temp_directory_path() / "msc_soak";
    }
    return SoakCheck(std::move(options)).run();
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
    }
    return 1;
}
