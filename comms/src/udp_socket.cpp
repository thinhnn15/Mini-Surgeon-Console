#include "msc/comms/udp_socket.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace msc::comms {
namespace {

#if defined(_WIN32)
// From <mstcpip.h>, which needs more of <windows.h> than this file wants to pull in.
constexpr DWORD kSioUdpConnReset = _WSAIOW(IOC_VENDOR, 12);

using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
int lastError() noexcept {
    return WSAGetLastError();
}
int closeNative(NativeSocket s) noexcept {
    return closesocket(s);
}
int pollOne(pollfd& fd, int timeoutMs) noexcept {
    return WSAPoll(&fd, 1, timeoutMs);
}
using SockLen = int;
using IoSize = int;

// Winsock must be started before the first socket and is reference counted. A function-local
// static starts it once, thread-safely, and stops it at exit.
bool winsockReady() noexcept {
    struct Winsock {
        bool ok = false;
        Winsock() noexcept {
            WSADATA data{};
            ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }
        ~Winsock() {
            if (ok) {
                WSACleanup();
            }
        }
        Winsock(const Winsock&) = delete;
        Winsock& operator=(const Winsock&) = delete;
        Winsock(Winsock&&) = delete;
        Winsock& operator=(Winsock&&) = delete;
    };
    static const Winsock winsock;
    return winsock.ok;
}
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
int lastError() noexcept {
    return errno;
}
int closeNative(NativeSocket s) noexcept {
    return ::close(s);
}
int pollOne(pollfd& fd, int timeoutMs) noexcept {
    return ::poll(&fd, 1, timeoutMs);
}
using SockLen = socklen_t;
using IoSize = std::size_t;
bool winsockReady() noexcept {
    return true;
}
#endif

NativeSocket native(std::uintptr_t handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

sockaddr_in toSockaddr(const Endpoint& endpoint) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    std::memcpy(&address.sin_addr, endpoint.address.data(), endpoint.address.size());  // network order
    return address;
}

Endpoint fromSockaddr(const sockaddr_in& address) noexcept {
    Endpoint endpoint;
    std::memcpy(endpoint.address.data(), &address.sin_addr, endpoint.address.size());
    endpoint.port = ntohs(address.sin_port);
    return endpoint;
}

// The casts the socket API forces on its callers, in one place. It takes every kind of address
// as a generic sockaddr* (sockaddr_in is laid out to be used that way), and Winsock takes
// buffers as char*.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
const sockaddr* generic(const sockaddr_in& address) noexcept {
    return reinterpret_cast<const sockaddr*>(&address);
}
sockaddr* generic(sockaddr_in& address) noexcept {
    return reinterpret_cast<sockaddr*>(&address);
}
const char* chars(std::span<const std::byte> data) noexcept {
    return reinterpret_cast<const char*>(data.data());
}
[[maybe_unused]] char* chars(std::span<std::byte> data) noexcept {
    return reinterpret_cast<char*>(data.data());
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

// All of `text` as a number no larger than `max`.
std::optional<unsigned> number(std::string_view text, unsigned max) noexcept {
    unsigned value = 0;
    const char* last = std::to_address(text.end());
    const auto [next, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (text.empty() || error != std::errc{} || next != last || value > max) {
        return std::nullopt;
    }
    return value;
}

std::string describe(const char* what, int code) {
    return std::string(what) + ": " + std::system_category().message(code) + " (" + std::to_string(code) +
           ")";
}

}  // namespace

std::optional<Endpoint> Endpoint::parse(std::string_view text) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<unsigned> port = number(text.substr(colon + 1), 65535);
    std::string_view host = text.substr(0, colon);
    Endpoint endpoint;
    for (std::size_t i = 0; i < endpoint.address.size(); ++i) {
        const bool last = i + 1 == endpoint.address.size();
        const std::size_t dot = last ? host.size() : host.find('.');
        if (dot == std::string_view::npos) {
            return std::nullopt;
        }
        const std::optional<unsigned> part = number(host.substr(0, dot), 255);
        if (!part) {
            return std::nullopt;
        }
        endpoint.address[i] = static_cast<std::uint8_t>(*part);
        host.remove_prefix(last ? dot : dot + 1);
    }
    if (!port) {
        return std::nullopt;
    }
    endpoint.port = static_cast<std::uint16_t>(*port);
    return endpoint;
}

std::string Endpoint::toString() const {
    return std::to_string(address[0]) + '.' + std::to_string(address[1]) + '.' + std::to_string(address[2]) +
           '.' + std::to_string(address[3]) + ':' + std::to_string(port);
}

std::optional<UdpSocket> UdpSocket::open(const Endpoint& local, std::string* error) {
    const auto fail = [error](const char* what, int code) -> std::optional<UdpSocket> {
        if (error != nullptr) {
            *error = describe(what, code);
        }
        return std::nullopt;
    };
    if (!winsockReady()) {
        return fail("WSAStartup", lastError());
    }
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        return fail("socket", lastError());
    }
    // From here the socket is closed by UdpSocket's destructor, also on the error paths.
    UdpSocket socket(static_cast<std::uintptr_t>(s), local);

#if defined(_WIN32)
    // Windows quirk: if a datagram we sent is answered with ICMP "port unreachable" (nobody
    // listening there, e.g. the console was restarted), the next recvfrom() on this socket
    // fails with WSAECONNRESET, although UDP has no connection. For a link that must keep
    // running while the other side comes and goes, that error is noise: switch it off.
    BOOL reportConnReset = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, kSioUdpConnReset, &reportConnReset, sizeof(reportConnReset), nullptr, 0, &returned, nullptr,
             nullptr);
#endif

    sockaddr_in address = toSockaddr(local);
    if (::bind(s, generic(address), sizeof(address)) != 0) {
        return fail("bind", lastError());
    }
    SockLen length = sizeof(address);
    if (::getsockname(s, generic(address), &length) != 0) {
        return fail("getsockname", lastError());
    }
    socket.local_ = fromSockaddr(address);
    return socket;
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_), local_(other.local_) {
    other.handle_ = kInvalid;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        local_ = other.local_;
        other.handle_ = kInvalid;
    }
    return *this;
}

UdpSocket::~UdpSocket() {
    close();
}

void UdpSocket::close() noexcept {
    if (handle_ != kInvalid) {
        closeNative(native(handle_));
        handle_ = kInvalid;
    }
}

// Not const, although the compiler would allow it: sending changes the socket's state in the OS.
// NOLINTNEXTLINE(readability-make-member-function-const)
bool UdpSocket::sendTo(std::span<const std::byte> data, const Endpoint& to) noexcept {
    const sockaddr_in address = toSockaddr(to);
    const auto sent = ::sendto(native(handle_), chars(data), static_cast<IoSize>(data.size()), 0,
                               generic(address), sizeof(address));
    return sent >= 0 && static_cast<std::size_t>(sent) == data.size();
}

// Not const either: receiving takes the datagram out of the socket's queue.
// NOLINTNEXTLINE(readability-make-member-function-const)
ReceiveResult UdpSocket::receive(std::span<std::byte> buffer, std::chrono::milliseconds timeout) noexcept {
    ReceiveResult result;
    pollfd fd{};
    fd.fd = native(handle_);
    fd.events = POLLIN;
    const int ready = pollOne(fd, static_cast<int>(timeout.count()));
    if (ready == 0) {
        return result;  // Timeout
    }
    if (ready < 0) {
        result.status = ReceiveStatus::Error;
        result.errorCode = lastError();
        return result;
    }

    sockaddr_in from{};
#if defined(_WIN32)
    SockLen fromLength = sizeof(from);
    const int received = ::recvfrom(native(handle_), chars(buffer), static_cast<int>(buffer.size()), 0,
                                    generic(from), &fromLength);
    if (received < 0) {
        const int code = lastError();
        if (code != WSAEMSGSIZE) {
            result.status = ReceiveStatus::Error;
            result.errorCode = code;
            return result;
        }
        // Windows fills the buffer with the start of the datagram and reports the rest as lost.
        result.truncated = true;
        result.size = buffer.size();
    } else {
        result.size = static_cast<std::size_t>(received);
    }
#else
    // recvmsg, not recvfrom: only it says (MSG_TRUNC in msg_flags) that the datagram was cut.
    iovec io{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = &from;
    message.msg_namelen = sizeof(from);
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    const ssize_t received = ::recvmsg(native(handle_), &message, 0);
    if (received < 0) {
        result.status = ReceiveStatus::Error;
        result.errorCode = lastError();
        return result;
    }
    result.size = static_cast<std::size_t>(received);
    result.truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
    result.status = ReceiveStatus::Datagram;
    result.from = fromSockaddr(from);
    return result;
}

}  // namespace msc::comms
