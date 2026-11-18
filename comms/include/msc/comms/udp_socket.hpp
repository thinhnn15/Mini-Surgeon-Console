#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace msc::comms {

// An IPv4 address and port. The arm link runs on one machine (loopback) or a dedicated
// network; IPv6 and name lookup are not needed.
struct Endpoint {
    std::array<std::uint8_t, 4> address{127, 0, 0, 1};
    std::uint16_t port = 0;

    [[nodiscard]] static Endpoint loopback(std::uint16_t port) noexcept {
        return Endpoint{{127, 0, 0, 1}, port};
    }
    // "a.b.c.d:port". Nothing if it is not exactly that.
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view text);
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

enum class ReceiveStatus : std::uint8_t {
    Datagram,  // one datagram arrived
    Timeout,   // nothing arrived in time
    Error,     // the OS reported an error (errorCode says which)
};

struct ReceiveResult {
    ReceiveStatus status = ReceiveStatus::Timeout;
    std::size_t size = 0;  // bytes in the buffer
    Endpoint from;
    // The datagram was larger than the buffer: only its start is in the buffer, the rest is
    // gone (UDP never splits a datagram over two receives). Not ours, or a protocol mismatch.
    bool truncated = false;
    int errorCode = 0;  // errno / WSAGetLastError() for Error
};

// A UDP socket bound to a local endpoint: send a datagram to an endpoint, receive one with a
// timeout. RAII; movable, not copyable. Winsock on Windows, BSD sockets elsewhere, behind the
// same interface. No Qt: arm_sim uses it too.
//
// Why UDP for the arm link: a lost packet is simply lost, and the next one (1 ms later) carries
// newer data anyway. TCP would hold back every later packet until the lost one is resent
// (head-of-line blocking): stale commands, delivered late, in a burst. What UDP does not
// guarantee (delivery, order, no duplicates, integrity beyond a weak checksum) is handled by
// the protocol: sequence numbers, SequenceFilter, CRC-32, heartbeats.
//
// One thread at a time per socket.
class UdpSocket {
public:
    // Binds to `local` (port 0: the OS picks a free one, see localEndpoint()). Nothing, with a
    // reason in `error` if given, if the socket cannot be created or bound (port in use).
    [[nodiscard]] static std::optional<UdpSocket> open(const Endpoint& local, std::string* error = nullptr);

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket();

    // The endpoint the socket is bound to, with the real port.
    [[nodiscard]] const Endpoint& localEndpoint() const noexcept { return local_; }

    // Sends one datagram. False if the OS refused it. True does not mean it arrived: UDP never
    // tells.
    bool sendTo(std::span<const std::byte> data, const Endpoint& to) noexcept;

    // Waits up to `timeout` for one datagram (0: just look) and copies it into `buffer`.
    [[nodiscard]] ReceiveResult receive(std::span<std::byte> buffer,
                                        std::chrono::milliseconds timeout) noexcept;

private:
    UdpSocket(std::uintptr_t handle, const Endpoint& local) noexcept : handle_(handle), local_(local) {}
    void close() noexcept;

    static constexpr std::uintptr_t kInvalid = ~std::uintptr_t{0};
    std::uintptr_t handle_ = kInvalid;  // SOCKET on Windows, int elsewhere
    Endpoint local_;
};

}  // namespace msc::comms
