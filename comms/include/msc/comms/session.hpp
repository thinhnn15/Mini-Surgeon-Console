#pragma once

#include <cstdint>

namespace msc::comms {

// A random, non-zero id for this process's packets (see SequenceFilter): different on every
// start, so a receiver can tell a restarted sender from a stream of old packets.
[[nodiscard]] std::uint32_t newSessionId();

}  // namespace msc::comms
