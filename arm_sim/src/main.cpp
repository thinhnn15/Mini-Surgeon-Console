#include <cstdio>
#include <span>
#include <string_view>

int main(int argc, char* argv[]) {
    const std::span args(argv, static_cast<std::size_t>(argc));
    for (const std::string_view arg : args.subspan(1)) {
        if (arg == "--version") {
            std::printf("arm_sim %s\n", MSC_VERSION);
            return 0;
        }
    }

    // M3: the 1 kHz joint loop, UDP link and heartbeat watchdog go here.
    std::printf("arm_sim %s: M1 skeleton, nothing to simulate yet.\n", MSC_VERSION);
    return 0;
}
