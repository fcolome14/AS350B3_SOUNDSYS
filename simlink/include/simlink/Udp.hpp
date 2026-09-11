// simlink/Udp.hpp - just enough UDP for one sender and one listener.
//
// No socket headers leak out of this file: winsock2.h in a public header
// fights with windows.h in whatever includes it second (SDL, NanoVG...), so
// the platform code lives in Udp.cpp behind plain integers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace simlink {

class UdpSender {
public:
    UdpSender() = default;
    ~UdpSender() { close(); }
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // host: an IPv4 address or a name ("127.0.0.1", "localhost", "raspberrypi.local").
    bool open(const std::string& host, std::uint16_t port, std::string* error = nullptr);

    // Fire and forget. Returns false only if the datagram could not be handed to
    // the OS; nobody listening is not an error - UDP does not know or care.
    bool send(const void* data, std::size_t size);

    bool isOpen() const { return socket_ != kInvalid; }
    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t socket_ = kInvalid;
    std::uint32_t addressBe_ = 0;  // IPv4, network byte order
    std::uint16_t portBe_ = 0;
};

class UdpReceiver {
public:
    UdpReceiver() = default;
    ~UdpReceiver() { close(); }
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // port 0 picks any free port (read it back with port()). "0.0.0.0" listens
    // on every interface, which is what a VEMD on another machine needs.
    bool open(std::uint16_t port, const std::string& bindAddress = "0.0.0.0",
              std::string* error = nullptr);

    // Waits up to timeoutMs for one datagram. Returns its size, 0 on timeout or
    // on a datagram that had to be discarded, -1 on a socket error.
    int receive(void* buffer, std::size_t capacity, int timeoutMs);

    std::uint16_t port() const { return port_; }
    bool isOpen() const { return socket_ != kInvalid; }
    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t socket_ = kInvalid;
    std::uint16_t port_ = 0;
};

// "host:port" -> host, port. A missing ":port" leaves `port` untouched. Returns
// false (and changes nothing) on an empty host or a port outside 1..65535.
bool parseEndpoint(const std::string& text, std::string& host, std::uint16_t& port);

}  // namespace simlink
