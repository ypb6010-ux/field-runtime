// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "SimEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint64_t xorshift(std::uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

double uniform01(Point& p) {
    if (!p.seeded) {
        p.rng = std::hash<std::string>{}(p.id) | 1ull;
        p.seeded = true;
    }
    return double(xorshift(p.rng) >> 11) / double(1ull << 53);
}

double uniform(Point& p, double a, double b) {
    return a + (b - a) * uniform01(p);
}

double gaussian(Point& p, double sigma) {
    double u1 = std::max(1e-12, uniform01(p));
    double u2 = uniform01(p);
    return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

double triangleWave(double phase01) {           // 0..1 phase -> 0..1 triangle
    return phase01 < 0.5 ? phase01 * 2.0 : 2.0 - phase01 * 2.0;
}

} // namespace

int Point::words() const {
    switch (type) {
        case Type::U32: case Type::I32: case Type::F32: return 2;
        default: return 1;
    }
}

double SimEngine::eval(Point& p, double t) {
    double const T = p.periodMs / 1000.0;
    double const phase = (T > 0) ? std::fmod(t, T) / T : 0.0;   // 0..1
    double v = 0.0;
    switch (p.pattern) {
        case Pattern::Constant:
            v = p.offset;
            break;
        case Pattern::Counter:
            p.current += p.step;
            if (p.wrap > 0 && p.current >= p.wrap) p.current = std::fmod(p.current, p.wrap);
            v = p.current;
            break;
        case Pattern::Sine:
            v = p.offset + p.amplitude * std::sin(2.0 * kPi * t / (T > 0 ? T : 1.0));
            break;
        case Pattern::Triangle:
            v = p.min + (p.max - p.min) * triangleWave(phase);
            break;
        case Pattern::Sawtooth:
            v = p.min + (p.max - p.min) * phase;
            break;
        case Pattern::Square:
            v = phase < p.duty ? p.max : p.min;
            break;
        case Pattern::RandomWalk:
            p.current += uniform(p, -p.step, p.step);
            p.current = std::clamp(p.current, p.min, p.max);
            v = p.current;
            break;
        case Pattern::UniformRandom:
            v = uniform(p, p.min, p.max);
            break;
        case Pattern::StepSequence: {
            if (p.values.empty()) { v = 0; break; }
            double const dwell = std::max(1.0, p.dwellMs) / 1000.0;
            std::size_t idx = std::size_t(std::llround(std::floor(t / dwell))) % p.values.size();
            v = p.values[idx];
            break;
        }
    }
    if (p.noiseSigma > 0 && p.pattern != Pattern::StepSequence && p.type != Type::Bool) {
        v += gaussian(p, p.noiseSigma);
    }
    return v;
}

std::vector<std::uint16_t> SimEngine::encode(Point const& p, double value) {
    auto split32 = [](std::uint32_t u) {
        return std::vector<std::uint16_t>{                 // big-endian hi_lo
            std::uint16_t(u >> 16), std::uint16_t(u & 0xFFFFu)};
    };
    switch (p.type) {
        case Type::Bool:
            return {std::uint16_t(value >= 0.5 ? 1 : 0)};
        case Type::U16:
            return {std::uint16_t(std::clamp(std::llround(value), 0ll, 65535ll))};
        case Type::I16:
            return {std::uint16_t(std::int16_t(std::clamp(std::llround(value), -32768ll, 32767ll)))};
        case Type::Enum:
            return {std::uint16_t(std::clamp(std::llround(value), 0ll, 65535ll))};
        case Type::U32:
            return split32(std::uint32_t(std::clamp(std::llround(value), 0ll, 4294967295ll)));
        case Type::I32:
            return split32(std::uint32_t(std::int32_t(std::clamp(
                std::llround(value), -2147483648ll, 2147483647ll))));
        case Type::F32: {
            float f = float(value);
            std::uint32_t u;
            std::memcpy(&u, &f, 4);
            return split32(u);
        }
    }
    return {0};
}

SimEngine SimEngine::defaultCatalog() {
    SimEngine e;
    using P = Pattern;
    using T = Type;
    using F = Face;
    auto mk = [](std::string id, F face, int addr, T type, P pat) {
        Point p; p.id = std::move(id); p.face = face; p.addr = addr; p.type = type; p.pattern = pat;
        return p;
    };

    // ── Modbus face (HR word index) ──
    { auto p = mk("sim.temperature", F::Modbus, 0, T::I16, P::Sine);
      p.offset = 250; p.amplitude = 80; p.periodMs = 30000; e.add(p); }       // *0.1 -> 17..33
    { auto p = mk("sim.pressure", F::Modbus, 1, T::U16, P::RandomWalk);
      p.current = 5000; p.step = 50; p.min = 4000; p.max = 6000; e.add(p); }
    { auto p = mk("sim.flow", F::Modbus, 2, T::F32, P::Sine);
      p.offset = 120; p.amplitude = 40; p.periodMs = 20000; e.add(p); }
    { auto p = mk("sim.run_state", F::Modbus, 4, T::Enum, P::StepSequence);
      p.values = {0,1,1,1,2}; p.dwellMs = 5000; e.add(p); }
    { auto p = mk("sim.total_count", F::Modbus, 5, T::U32, P::Counter);
      p.step = 1; p.wrap = 1000000; e.add(p); }
    { auto p = mk("sim.alarm", F::Modbus, 7, T::Bool, P::Square);
      p.periodMs = 60000; p.duty = 0.1; p.min = 0; p.max = 1; e.add(p); }

    // ── S7 face (DB word index) ──
    { auto p = mk("sim.s7.motor_temp", F::S7, 0, T::I16, P::Sine);
      p.offset = 400; p.amplitude = 120; p.periodMs = 35000; e.add(p); }
    { auto p = mk("sim.s7.torque", F::S7, 1, T::F32, P::RandomWalk);
      p.current = 85; p.step = 3; p.min = 40; p.max = 140; e.add(p); }
    { auto p = mk("sim.s7.cycle", F::S7, 3, T::U32, P::Counter);
      p.step = 1; p.wrap = 10000000; e.add(p); }

    // ── OPC UA face (node Sim_<addr>) ──
    { auto p = mk("sim.opc.level", F::OpcUa, 0, T::F32, P::Triangle);
      p.min = 0; p.max = 100; p.periodMs = 40000; e.add(p); }
    { auto p = mk("sim.opc.speed", F::OpcUa, 1, T::I32, P::RandomWalk);
      p.current = 1450; p.step = 20; p.min = 1000; p.max = 1800; e.add(p); }
    { auto p = mk("sim.opc.valve", F::OpcUa, 2, T::Bool, P::Square);
      p.periodMs = 30000; p.duty = 0.5; p.min = 0; p.max = 1; e.add(p); }

    return e;
}

} // namespace sim
