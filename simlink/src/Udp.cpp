#include "simlink/Udp.hpp"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace simlink {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
const SocketHandle kBadSocket = INVALID_SOCKET;

// Winsock needs one WSAStartup per process before any socket call. A function
// static runs it exactly once, thread-safely, and WSACleanup at exit.
struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};
bool socketsReady() {
    static WinsockInit init;
    return init.ok;
}
void closeSocket(SocketHandle s) { closesocket(s); }
std::string socketError(const char* what) {
    return std::string(what) + " failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
}
#else
using SocketHandle = int;
const SocketHandle kBadSocket = -1;
bool socketsReady() { return true; }
void closeSocket(SocketHandle s) { ::close(s); }
std::string socketError(const char* what) {
    return std::string(what) + " failed (" + std::strerror(errno) + ")";
}
#endif

SocketHandle handleOf(std::intptr_t v) { return static_cast<SocketHandle>(v); }
std::intptr_t valueOf(SocketHandle s) {
    return s == kBadSocket ? -1 : static_cast<std::intptr_t>(s);
}

}  // namespace

// ---------------------------------------------------------------- sender
bool UdpSender::open(const std::string& host, std::uint16_t port, std::string* error) {
    close();
    if (!socketsReady()) {
        if (error) *error = "socket subsystem unavailable";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        if (error) *error = "could not resolve " + host;
        return false;
    }
    sockaddr_in addr{};
    std::memcpy(&addr, found->ai_addr, sizeof(addr));
    freeaddrinfo(found);

    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) {
        if (error) *error = socketError("socket");
        return false;
    }
    socket_ = valueOf(s);
    addressBe_ = addr.sin_addr.s_addr;
    portBe_ = htons(port);
    return true;
}

bool UdpSender::send(const void* data, std::size_t size) {
    if (socket_ == kInvalid) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = addressBe_;
    addr.sin_port = portBe_;
#if defined(_WIN32)
    const int n = ::sendto(handleOf(socket_), static_cast<const char*>(data),
                           static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&addr),
                           sizeof(addr));
#else
    const ssize_t n = ::sendto(handleOf(socket_), data, size, 0,
                               reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
#endif
    return n >= 0 && static_cast<std::size_t>(n) == size;
}

void UdpSender::close() {
    if (socket_ != kInvalid) {
        closeSocket(handleOf(socket_));
        socket_ = kInvalid;
    }
}

// -------------------------------------------------------------- receiver
bool UdpReceiver::open(std::uint16_t port, const std::string& bindAddress, std::string* error) {
    close();
    if (!socketsReady()) {
        if (error) *error = "socket subsystem unavailable";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        if (error) *error = "not an IPv4 address: " + bindAddress;
        return false;
    }

    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) {
        if (error) *error = socketError("socket");
        return false;
    }
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = socketError("bind") + " on port " + std::to_string(port);
        closeSocket(s);
        return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    socket_ = valueOf(s);
    return true;
}

int UdpReceiver::receive(void* buffer, std::size_t capacity, int timeoutMs) {
    if (socket_ == kInvalid) return -1;
    const SocketHandle s = handleOf(socket_);

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
    if (ready < 0) return -1;
    if (ready == 0) return 0;
    const int n = ::recvfrom(s, static_cast<char*>(buffer), static_cast<int>(capacity), 0,
                             nullptr, nullptr);
    if (n < 0) {
        // Oversized datagram (not ours) or a stray ICMP report: drop, keep going.
        const int e = WSAGetLastError();
        return (e == WSAEMSGSIZE || e == WSAECONNRESET) ? 0 : -1;
    }
    return n;
#else
    const int ready = ::select(s + 1, &readable, nullptr, nullptr, &tv);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready == 0) return 0;
    const ssize_t n = ::recvfrom(s, buffer, capacity, 0, nullptr, nullptr);
    if (n < 0) return errno == EINTR ? 0 : -1;
    return static_cast<int>(n);
#endif
}

void UdpReceiver::close() {
    if (socket_ != kInvalid) {
        closeSocket(handleOf(socket_));
        socket_ = kInvalid;
    }
    port_ = 0;
}

// ---------------------------------------------------------------- helpers
bool parseEndpoint(const std::string& text, std::string& host, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    const std::string h = (colon == std::string::npos) ? text : text.substr(0, colon);
    if (h.empty()) return false;
    if (colon == std::string::npos) {
        host = h;
        return true;
    }
    const std::string digits = text.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    unsigned long value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    if (value < 1 || value > 65535) return false;
    host = h;
    port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace simlink
