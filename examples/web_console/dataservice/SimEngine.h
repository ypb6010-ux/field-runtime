// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Hardware-free data simulation for the web_console example. A SimEngine holds a
// catalog of points; each point has a data type + a variation pattern. On every
// tick it evaluates the pattern at the current wall-clock time and encodes the
// result into raw 16-bit register words (big-endian), which the protocol faces
// (Modbus / S7 / OPC UA) expose. See docs/DATA_SERVICE.md for the data dictionary.
namespace sim {

enum class Type { Bool, U16, I16, U32, I32, F32, Enum };

enum class Pattern {
    Constant, Counter, Sine, Triangle, Sawtooth, Square,
    RandomWalk, UniformRandom, StepSequence,
};

// Which protocol face a point belongs to, and where it sits there.
enum class Face { Modbus, S7, OpcUa };

struct Point {
    std::string id;
    Face        face = Face::Modbus;
    int         addr = 0;          // word index within the face (S7: byte = addr*2)
    Type        type = Type::U16;

    Pattern     pattern = Pattern::Constant;
    double      offset = 0;        // sine/constant base
    double      amplitude = 0;     // sine
    double      min = 0;
    double      max = 0;
    double      periodMs = 1000;   // time-based patterns
    double      step = 1;          // counter / random_walk
    double      wrap = 0;          // counter wrap (0 = no wrap)
    double      duty = 0.5;        // square
    double      noiseSigma = 0;    // optional gaussian overlay
    double      dwellMs = 1000;    // step_sequence
    std::vector<double> values;    // step_sequence discrete values

    // Mutable state for stateful patterns.
    double        current = 0;     // counter / random_walk accumulator
    std::uint64_t rng = 0;         // xorshift state (seeded from id)
    bool          seeded = false;

    int words() const;             // 1 (bool/u16/i16/enum) or 2 (u32/i32/f32)
};

class SimEngine {
public:
    // The built-in "factory" catalog (mirrors docs/DATA_SERVICE.md §4).
    static SimEngine defaultCatalog();

    void add(Point p) { m_points.push_back(std::move(p)); }
    std::vector<Point>& points() { return m_points; }
    std::vector<Point> const& points() const { return m_points; }

    // Evaluate one point at `tSeconds` (wall clock since start) -> engineering
    // value, advancing any internal state.
    static double eval(Point& p, double tSeconds);

    // Encode an engineering value into raw register words for the point's type.
    static std::vector<std::uint16_t> encode(Point const& p, double value);

private:
    std::vector<Point> m_points;
};

} // namespace sim
