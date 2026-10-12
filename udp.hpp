#pragma once
#include "telemetry.hpp"
#include <bit>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace apex {
// Explicit little-endian reads avoid packing, alignment, and aliasing pitfalls.
// All access is bounded, including names and fields we currently do not render.
class Bytes {
public:
    explicit Bytes(std::span<const std::uint8_t> bytes) : b_(bytes) {}
    std::uint8_t u8(std::size_t p) const { check(p, 1); return b_[p]; }
    int i8(std::size_t p) const { return std::bit_cast<std::int8_t>(u8(p)); }
    std::uint16_t u16(std::size_t p) const { check(p, 2); return b_[p] | (std::uint16_t(b_[p+1]) << 8); }
    int i16(std::size_t p) const { return std::bit_cast<std::int16_t>(u16(p)); }
    std::uint32_t u32(std::size_t p) const { return u16(p) | (std::uint32_t(u16(p+2)) << 16); }
    std::uint64_t u64(std::size_t p) const { return u32(p) | (std::uint64_t(u32(p+4)) << 32); }
    float f32(std::size_t p) const {
        float v = std::bit_cast<float>(u32(p));
        if (!std::isfinite(v)) throw std::runtime_error("Non-finite telemetry");
        return v;
    }
    std::string name(std::size_t p) const {
        check(p, 32);
        std::string s;
        for (std::size_t i = 0; i < 32 && b_[p+i]; ++i)
            if (b_[p+i] >= 32 && b_[p+i] != 127) s += static_cast<char>(b_[p+i]);
        return s.empty() ? "DRIVER" : s;
    }
private:
    void check(std::size_t p, std::size_t n) const {
        if (p > b_.size() || n > b_.size() - p) throw std::runtime_error("Truncated telemetry");
    }
    std::span<const std::uint8_t> b_;
};

class Decoder {
public:
    Telemetry data;
    std::uint64_t accepted = 0, rejected = 0;
    std::uint64_t epoch = 0; // Changes when the active session, player, or format changes.
    std::string warning;
    std::array<double, 17> updated{};
    Decoder() { updated.fill(-1); }
    bool fresh(int id, double now, double seconds = 2.0) const {
        return updated[id] >= 0 && now - updated[id] < seconds;
    }
    bool parse(std::span<const std::uint8_t> packet, double now) {
        try {
            const Bytes b(packet);
            const int year = b.u16(0), version = b.u8(5), id = b.u8(6), player = b.u8(27);
            if (year != 2025 && year != 2026) return reject("Unsupported UDP format: " + std::to_string(year));
            const int cars = year == 2026 ? 24 : 22;
            if (player >= cars) return reject("Invalid player car index");
            // Unused packets are ignored; a new version of a consumed packet is
            // rejected rather than silently reinterpreted as a known layout.
            const int required = packetSize(year, id);
            if (!required) return false;
            if (version != 1) return reject("Unsupported packet version: " + std::to_string(version));
            if (packet.size() < static_cast<std::size_t>(required)) return reject("Truncated UDP packet");
            const auto uid = b.u64(7);
            const auto frame = b.u32(23); // Monotonic even when the player uses a flashback.
            const bool newSession = !sessionSet_ || uid != session_ || player != player_ || year != data.format;
            if (!newSession && seen_[id] && std::bit_cast<std::int32_t>(frame - frames_[id]) < 0) return false;

            // Parse transactionally. A malformed datagram never partially updates
            // the displayed state, frame counters, or active session identity.
            Telemetry next = newSession ? Telemetry{} : data;
            auto names = newSession ? std::array<std::string, 24>{} : names_;
            next.format = year;
            const std::size_t p = static_cast<std::size_t>(player);
            constexpr std::array<int, 4> wheel{2, 3, 0, 1};
            if (id == 0) {
                const std::size_t o = 29 + p * (year == 2026 ? 54 : 60);
                next.lateralG = year == 2026 ? b.i16(o+36) / 1000.0f : b.f32(o+36);
                next.longitudinalG = year == 2026 ? b.i16(o+38) / 1000.0f : b.f32(o+40);
                next.hasMotion = true;
            } else if (id == 1) {
                next.trackTemp = static_cast<float>(b.i8(30)); next.airTemp = static_cast<float>(b.i8(31));
                next.totalLaps = b.u8(32); next.trackLength = static_cast<float>(b.u16(33));
                next.trackId = b.i8(36); next.formula = b.u8(37);
                // Packet format 2026 can also carry a pre-2026 car.
                next.regs2026 = next.formula == 13;
                next.hasSession = true;
            } else if (id == 2) {
                const std::size_t o = 29 + p * 57;
                next.lastLap = b.u32(o) / 1000.0f;
                next.lapSeconds = b.u32(o+4) / 1000.0f;
                next.sectors[0] = b.u16(o+8) / 1000.0f + b.u8(o+10) * 60.0f;
                next.sectors[1] = b.u16(o+11) / 1000.0f + b.u8(o+13) * 60.0f;
                next.gapAhead = b.u16(o+14) / 1000.0f + b.u8(o+16) * 60.0f;
                next.lapDistance = b.f32(o+20);
                next.position = b.u8(o+32); next.lap = b.u8(o+33);
                next.sector = std::min(2, int(b.u8(o+36)));
                next.invalidLap = b.u8(o+37) != 0;
                next.penalties = b.u8(o+38); next.warnings = b.u8(o+40);
                next.gapBehind = 0; next.aheadName = next.behindName = "---";
                for (int i = 0; i < cars; ++i) {
                    const std::size_t other = 29 + i * 57;
                    const int position = b.u8(other+32);
                    if (position > 0 && position == next.position - 1) next.aheadName = names[i];
                    if (next.position > 0 && position == next.position + 1) {
                        next.behindName = names[i];
                        next.gapBehind = b.u16(other+14) / 1000.0f + b.u8(other+16) * 60.0f;
                    }
                }
                next.hasLap = true;
            } else if (id == 4) {
                const int count = std::min(cars, int(b.u8(29)));
                const int stride = year == 2026 ? 60 : 57, offset = year == 2026 ? 10 : 7;
                for (int i = 0; i < count; ++i) names[i] = b.name(30 + i * stride + offset);
                if (player < count) next.driver = names[player];
            } else if (id == 6) {
                const std::size_t o = 29 + p * (year == 2026 ? 59 : 60);
                next.speed = static_cast<float>(b.u16(o));
                next.throttle = clamp01(b.f32(o+2)); next.steer = std::clamp(b.f32(o+6), -1.0f, 1.0f);
                next.brake = clamp01(b.f32(o+10)); next.gear = std::clamp(b.i8(o+15), -1, 8);
                next.clutch = clamp01(b.u8(o+14) / 100.0f);
                next.rpm = static_cast<float>(b.u16(o+16)); next.revPercent = static_cast<float>(b.u8(o+19));
                next.drs = b.u8(o+18) ? Drs::Open : (drsAllowed_ && !newSession ? Drs::Available : Drs::Closed);
                next.engineTemp = static_cast<float>(year == 2026 ? b.u8(o+38) : b.u16(o+38));
                for (int i = 0; i < 4; ++i) {
                    next.brakeTemp[i] = static_cast<float>(b.u16(o+22+wheel[i]*2));
                    next.tyreTemp[i] = static_cast<float>(b.u8(o+34+wheel[i])); // Carcass temperature.
                    next.tyrePressure[i] = b.f32(o+(year == 2026 ? 39 : 40)+wheel[i]*4);
                }
                next.hasTelemetry = true;
            } else if (id == 7) {
                const std::size_t o = 29 + p * (year == 2026 ? 59 : 55);
                next.brakeBias = static_cast<float>(b.u8(o+3)); next.pitLimiter = b.u8(o+4) != 0;
                next.fuel = b.f32(o+5); next.fuelLaps = b.f32(o+13);
                next.compound = b.u8(o+26); next.tyreAge = b.u8(o+27); next.flag = b.i8(o+28);
                next.ersPercent = std::clamp(b.f32(o+37) / 40000.0f, 0.0f, 100.0f);
                next.ersMode = b.u8(o+41);
                next.deployedMJ = b.f32(o+(year == 2026 ? 54 : 50)) / 1'000'000.0f;
                if (next.drs != Drs::Open) next.drs = b.u8(o+22) ? Drs::Available : Drs::Closed;
                next.hasStatus = true;
            } else if (id == 10) {
                const std::size_t o = 29 + p * 46;
                for (int i = 0; i < 4; ++i) next.tyreWear[i] = std::clamp(b.f32(o+wheel[i]*4), 0.0f, 100.0f);
                next.hasDamage = true;
            } else if (id == 11) {
                if (b.u8(29) != player) return false;
                const int laps = std::min(100, int(b.u8(30)));
                const int best = b.u8(32);
                next.bestLap = 0;
                if (best >= 1 && best <= laps) {
                    const std::size_t o = 36 + (best - 1) * 14;
                    if (b.u8(o+13) & 1) next.bestLap = b.u32(o) / 1000.0f;
                }
                for (int s = 0; s < 3; ++s) {
                    const int lap = b.u8(33+s);
                    next.bestSectors[s] = 0;
                    if (lap >= 1 && lap <= laps) {
                        const std::size_t o = 36 + (lap - 1) * 14;
                        if (b.u8(o+13) & (2 << s))
                            next.bestSectors[s] = b.u16(o+4+s*3) / 1000.0f + b.u8(o+6+s*3) * 60.0f;
                    }
                }
                next.hasHistory = true;
            } else if (id == 16) {
                const std::size_t o = 29 + p * 10;
                next.straightMode = b.u8(o) == 1; next.aeroAvailable = b.u8(o+1) == 1;
                next.overtakeAvailable = b.u8(o+4) == 1; next.overtake = b.u8(o+5) == 1;
                next.regs2026 = b.u8(o+8) == 1; next.aeroKnown = true;
            }
            if (newSession) {
                ++epoch;
                updated.fill(-1); seen_.fill(false); drsAllowed_ = false;
                session_ = uid; sessionSet_ = true; player_ = player;
            }
            if (id == 7) drsAllowed_ = b.u8(29 + p * (year == 2026 ? 59 : 55) + 22) != 0;
            data = std::move(next); names_ = std::move(names);
            updated[id] = now; frames_[id] = frame; seen_[id] = true;
            ++accepted; warning.clear();
            return true;
        } catch (const std::exception&) { return reject("Malformed UDP packet"); }
    }
    static int packetSize(int year, int id) {
        const int cars = year == 2026 ? 24 : 22;
        switch (id) {
            case 0: return 29 + cars * (year == 2026 ? 54 : 60);
            case 1: return year == 2026 ? 926 : 753;
            case 2: return 31 + cars * 57;
            case 4: return 30 + cars * (year == 2026 ? 60 : 57);
            case 6: return 32 + cars * (year == 2026 ? 59 : 60);
            case 7: return 29 + cars * (year == 2026 ? 59 : 55);
            case 10: return 29 + cars * 46;
            case 11: return 1460;
            case 16: return year == 2026 ? 269 : 0;
            default: return 0;
        }
    }
private:
    bool reject(std::string s) { ++rejected; warning = std::move(s); return false; }
    std::array<std::string, 24> names_{};
    std::array<std::uint32_t, 17> frames_{};
    std::array<bool, 17> seen_{};
    std::uint64_t session_ = 0;
    int player_ = -1;
    bool sessionSet_ = false, drsAllowed_ = false;
};

// Non-blocking receive on the render thread: no locks, worker lifetime races, or
// unbounded queues. The per-frame receive budget prevents network starvation.
class UdpReceiver {
public:
    Decoder decoder;
    std::string error;
    UdpReceiver() = default;
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    ~UdpReceiver() { close(); }
    bool open(int port) {
        close(); error.clear(); decoder = Decoder{};
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { error = "Winsock initialization failed"; return false; }
        winsock_ = true;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid) return fail("Cannot create UDP socket");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return fail("Cannot bind UDP " + std::to_string(port) + ". Port may be in use.");
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) return fail("Cannot enable non-blocking UDP");
#else
        if (fcntl(socket_, F_SETFL, O_NONBLOCK) == -1) return fail("Cannot enable non-blocking UDP");
#endif
        return true;
    }
    void poll(double now) {
        if (socket_ == invalid) return;
        std::array<std::uint8_t, 8192> buffer{};
        for (int i = 0; i < 512; ++i) {
            const int n = static_cast<int>(::recv(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0));
            if (n < 0) {
#ifdef _WIN32
                const int code = WSAGetLastError();
                if (code == WSAEMSGSIZE) { ++decoder.rejected; continue; }
                if (code != WSAEWOULDBLOCK) error = "UDP receive error " + std::to_string(code);
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK) error = "UDP receive error " + std::to_string(errno);
#endif
                break;
            }
            decoder.parse({buffer.data(), static_cast<std::size_t>(n)}, now);
        }
    }
    int localPort() const {
        if (socket_ == invalid) return 0;
        sockaddr_in address{};
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        return getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0 ? ntohs(address.sin_port) : 0;
    }
    void close() {
        if (socket_ != invalid) {
#ifdef _WIN32
            closesocket(socket_);
#else
            ::close(socket_);
#endif
            socket_ = invalid;
        }
#ifdef _WIN32
        if (winsock_) { WSACleanup(); winsock_ = false; }
#endif
    }
private:
    bool fail(const std::string& s) { error = s; close(); return false; }
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket invalid = INVALID_SOCKET;
    bool winsock_ = false;
#else
    using Socket = int;
    static constexpr Socket invalid = -1;
#endif
    Socket socket_ = invalid;
};
} // namespace apex
