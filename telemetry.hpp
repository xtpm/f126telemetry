#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace apex {
constexpr float pi = 3.14159265359f;
inline float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float approach(float x, float target, float rate, float dt) {
    return x + (target - x) * (1.0f - std::exp(-rate * dt));
}
inline std::string number(float value, int decimals = 1, bool sign = false) {
    char out[64];
    std::snprintf(out, sizeof(out), sign ? "%+.*f" : "%.*f", decimals, static_cast<double>(value));
    return out;
}
inline std::string lapTime(float seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return "--:--.---";
    const int ms = static_cast<int>(seconds * 1000.0f + 0.5f);
    char out[32];
    std::snprintf(out, sizeof(out), "%d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return out;
}
enum class Drs { Closed, Available, Open };

// This is the sole UI data model. Live packets and the demo update separate
// instances, so simulated values can never leak into a live session.
struct Telemetry {
    std::string driver = "WAITING FOR DRIVER", aheadName = "---", behindName = "---";
    int format = 0, position = 0, lap = 0, totalLaps = 0, sector = 0, gear = 0;
    int trackId = -1, formula = -1, compound = 0, tyreAge = 0, ersMode = 0, flag = 0;
    int penalties = 0, warnings = 0;
    float speed = 0, throttle = 0, brake = 0, clutch = 0, steer = 0, rpm = 0, revPercent = 0;
    float lapSeconds = 0, lastLap = 0, bestLap = 0, lapDistance = 0, trackLength = 0;
    float gapAhead = 0, gapBehind = 0, fuel = 0, fuelLaps = 0, brakeBias = 0;
    float ersPercent = 0, deployedMJ = 0, engineTemp = 0, trackTemp = 0, airTemp = 0;
    float lateralG = 0, longitudinalG = 0;
    std::array<float, 3> sectors{}, bestSectors{};
    // UI order is FL, FR, RL, RR; the decoder remaps the game's RL, RR, FL, FR.
    std::array<float, 4> tyreTemp{}, tyrePressure{}, tyreWear{}, brakeTemp{};
    Drs drs = Drs::Closed;
    bool regs2026 = false, aeroKnown = false, straightMode = false, aeroAvailable = false;
    bool overtake = false, overtakeAvailable = false, pitLimiter = false, invalidLap = false;
    bool hasTelemetry = false, hasLap = false, hasStatus = false, hasDamage = false;
    bool hasSession = false, hasMotion = false, hasHistory = false;
};

struct Sample { double time; float throttle, brake; };
class History {
public:
    static constexpr std::size_t capacity = 1440; // 24 seconds at 60 Hz, bounded memory.
    void clear() { next_ = count_ = 0; }
    void push(double time, float throttle, float brake) {
        samples_[next_] = {time, throttle, brake};
        next_ = (next_ + 1) % capacity;
        count_ = std::min(count_ + 1, capacity);
    }
    std::size_t size() const { return count_; }
    const Sample& at(std::size_t i) const { return samples_[(next_ + capacity - count_ + i) % capacity]; }
private:
    std::array<Sample, capacity> samples_{};
    std::size_t next_ = 0, count_ = 0;
};

class Simulator {
public:
    Telemetry data;
    double clock = 0;
    explicit Simulator(int year = 2026) { reset(year); }
    void reset(int year) {
        data = {};
        clock = 0;
        shiftCut_ = 0;
        data.driver = "ALEX MORGAN";
        data.aheadName = "NORRIS";
        data.behindName = "LECLERC";
        data.format = year;
        data.regs2026 = year == 2026;
        data.aeroKnown = true;
        data.position = 4; data.lap = 8; data.totalLaps = 27;
        data.trackLength = 5200; data.trackTemp = 34; data.airTemp = 24;
        data.speed = 190; data.gear = 5;
        data.lastLap = 88.417f; data.bestLap = 87.936f;
        data.sectors = {28.4f, 32.0f, 27.9f};
        data.bestSectors = {28.108f, 31.954f, 27.874f};
        data.lapSeconds = 53.4f; data.sector = 1;
        data.fuel = 32.4f; data.fuelLaps = 1.3f; data.brakeBias = 56;
        data.ersPercent = 76; data.compound = 17; data.tyreAge = 6;
        data.tyreTemp = {91, 89, 94, 93};
        data.tyrePressure = {27.5f, 27.3f, 24.4f, 24.3f};
        data.tyreWear = {16, 15, 20, 19};
        data.hasTelemetry = data.hasStatus = data.hasLap = data.hasSession = true;
        data.hasDamage = data.hasMotion = data.hasHistory = true;
    }
    void step(float dt) {
        // Stable fixed-step simulation is driven by the application at 120 Hz.
        clock += dt;
        const float t = static_cast<float>(clock);
        const float phase = std::fmod(t, 23.0f);
        const float oldSpeed = data.speed;
        float target = 0, throttle = 0, brake = 0;
        if (phase < 10) { // Long acceleration; aero opens after the detection point.
            target = 342; throttle = 1;
        } else if (phase < 12.3f) {
            // Initial pressure spike, then progressive trail braking into the
            // corner. This produces the familiar peaked red telemetry trace.
            const float release = (phase - 10.0f) / 2.3f;
            target = 82;
            brake = 0.98f * std::pow(1.0f - release, 0.65f);
        } else if (phase < 15.2f) {
            target = 115; throttle = 0.35f + 0.1f * std::sin(t * 2);
        } else if (phase < 20) {
            target = 268; throttle = 1;
        } else if (phase < 21.3f) {
            target = 140; brake = 0.78f * std::pow(1.0f - (phase - 20.0f) / 1.3f, 0.75f);
        } else { target = 178; throttle = 0.64f; }
        data.throttle = approach(data.throttle, throttle, 14, dt);
        data.brake = approach(data.brake, brake, 20, dt);
        data.speed = approach(data.speed, target, brake > 0 ? 1.6f : 0.27f, dt);
        data.steer = approach(data.steer, (phase > 12.1f && phase < 15.3f) ?
            0.34f * std::sin((phase - 12.1f) * pi / 3.2f) : (phase > 21 ? -0.17f : 0.005f), 5, dt);
        constexpr std::array<float, 8> shiftSpeed{80, 117, 156, 196, 235, 273, 310, 355};
        int gear = data.gear;
        if (gear < 8 && data.speed > shiftSpeed[gear - 1]) ++gear;
        if (gear > 1 && data.speed < shiftSpeed[gear - 2] - 12) --gear;
        const bool shifting = gear != data.gear;
        data.gear = gear;
        if (shifting) shiftCut_ = 0.065f;
        shiftCut_ = std::max(0.0f, shiftCut_ - dt);
        if (shiftCut_ > 0 && brake == 0) data.throttle *= 0.80f;
        const float low = gear == 1 ? 0 : shiftSpeed[gear - 2] - 12;
        data.rpm = std::clamp(9300 + 2950 * (data.speed - low) / (shiftSpeed[gear - 1] - low), 5200.0f, 12700.0f);
        data.revPercent = std::clamp((data.rpm - 9500) / 28.0f, 0.0f, 100.0f);
        data.aeroAvailable = phase > 3 && phase < 10;
        data.straightMode = data.regs2026 && phase > 4.2f && phase < 10;
        data.drs = data.regs2026 ? Drs::Closed : phase > 4.2f && phase < 10 ? Drs::Open :
            data.aeroAvailable ? Drs::Available : Drs::Closed;
        data.overtakeAvailable = data.regs2026 && phase > 2 && phase < 10;
        data.overtake = data.overtakeAvailable && phase > 5 && data.ersPercent > 18;
        const float regen = data.brake * 4.1f + (data.throttle < 0.6f ? 0.6f : 0.0f);
        const float deploy = data.throttle * (data.overtake ? 2.1f : 0.85f);
        data.ersPercent = std::clamp(data.ersPercent + (regen - deploy) * dt, 5.0f, 100.0f);
        data.ersMode = data.brake > 0.3f ? 0 : data.overtake ? 3 : 1;
        data.deployedMJ += deploy * 0.04f * dt;
        data.lapSeconds += dt;
        const int sector = data.lapSeconds < 28.4f ? 0 : data.lapSeconds < 60.4f ? 1 : 2;
        if (sector != data.sector) {
            if (sector == 1) data.sectors[0] = 28.4f;
            if (sector == 2) data.sectors[1] = 32.0f;
            data.sector = sector;
        }
        if (data.lapSeconds >= 88.3f) {
            data.lastLap = data.lapSeconds;
            data.bestLap = std::min(data.bestLap, data.lastLap);
            data.sectors[2] = data.lastLap - data.sectors[0] - data.sectors[1];
            data.lapSeconds -= 88.3f; ++data.lap; ++data.tyreAge;
            if (data.lap > data.totalLaps) data.lap = 1;
            data.deployedMJ = 0;
            data.sector = 0;
        }
        data.lapDistance = data.lapSeconds / 88.3f * data.trackLength;
        data.gapAhead = 0.79f + 0.27f * std::sin(t * 0.16f);
        data.gapBehind = 1.38f + 0.38f * std::sin(t * 0.13f + 1);
        data.longitudinalG = (data.speed - oldSpeed) / (3.6f * dt * 9.81f);
        data.lateralG = data.steer * data.speed * 0.09f;
        data.fuel = std::max(0.0f, data.fuel - dt * data.throttle * 0.018f);
        data.engineTemp = 107 + 3 * std::sin(t * 0.12f);
        for (int i = 0; i < 4; ++i) {
            const float load = std::abs(data.steer) * (i % 2 == 0 ? 45 : 28);
            const float temp = 87 + (i >= 2 ? 5.0f : 0.0f) + data.brake * 13 + load;
            data.tyreTemp[i] = approach(data.tyreTemp[i], temp, 0.12f, dt);
            data.brakeTemp[i] = approach(data.brakeTemp[i], 290 + data.brake * 770, 0.9f, dt);
            data.tyreWear[i] = std::min(100.0f, data.tyreWear[i] + dt * (0.004f + load * 0.001f));
        }
    }
private:
    float shiftCut_ = 0;
};
} // namespace apex
