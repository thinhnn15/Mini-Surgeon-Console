#include "msc/comms/session.hpp"

#include <chrono>
#include <cstdint>
#include <random>

namespace msc::comms {

std::uint32_t newSessionId() {
    // random_device is the OS's entropy source on the platforms we build for. Mixing in the
    // time guards against a platform where it is a fixed-seed generator (old MinGW did that).
    std::random_device device;
    const auto time = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::seed_seq seed{device(), device(), static_cast<std::uint32_t>(time),
                       static_cast<std::uint32_t>(time >> 32)};
    std::mt19937 generator(seed);
    std::uint32_t id = 0;
    while (id == 0) {
        id = static_cast<std::uint32_t>(generator());
    }
    return id;
}

}  // namespace msc::comms
