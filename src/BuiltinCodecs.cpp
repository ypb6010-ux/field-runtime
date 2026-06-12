// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/codec/BuiltinCodecs.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "core/dp/PortRef.h"
#include "core/dp/WordOrder.h"

namespace core::codec {

namespace {

// Pack two-register-wide network bytes from core::RegisterWords. Each Modbus
// register is big-endian on the wire; we record the network-order bytes in
// the order they would appear over TCP (i.e. position 0 = high byte of the
// first register), then apply the WordOrder permutation.
std::uint64_t unpackInt(core::RegisterWords const& raw,
                        int                        regCount,
                        dp::WordOrder              wordOrder) {
    if (regCount == 1) {
        return raw[0];
    }
    int const               byteCount = regCount * 2;
    std::array<std::uint8_t, 8> network{};
    for (int i = 0; i < regCount; ++i) {
        std::uint16_t const r = raw[i];
        network[2 * i]     = std::uint8_t(r >> 8);
        network[2 * i + 1] = std::uint8_t(r & 0xFF);
    }
    auto const perm = dp::permutationFor(wordOrder, byteCount);
    std::uint64_t result = 0;
    for (int i = 0; i < byteCount; ++i) {
        result = (result << 8) | network[perm.order[i]];
    }
    return result;
}

core::RegisterWords packInt(std::uint64_t value,
                            int           regCount,
                            dp::WordOrder wordOrder) {
    if (regCount == 1) {
        return {std::uint16_t(value & 0xFFFFu)};
    }
    int const               byteCount = regCount * 2;
    std::array<std::uint8_t, 8> result{};
    for (int i = 0; i < byteCount; ++i) {
        result[i] = std::uint8_t(value >> (8 * (byteCount - 1 - i)));
    }
    // Inverse permutation: network[perm[i]] = result[i]
    auto const             perm = dp::permutationFor(wordOrder, byteCount);
    std::array<std::uint8_t, 8> network{};
    for (int i = 0; i < byteCount; ++i) {
        network[perm.order[i]] = result[i];
    }
    core::RegisterWords out;
    out.reserve(regCount);
    for (int i = 0; i < regCount; ++i) {
        std::uint16_t r = std::uint16_t(network[2 * i]) << 8;
        r |= std::uint16_t(network[2 * i + 1]);
        out.push_back(r);
    }
    return out;
}

bool hasLinearTransform(dp::PortRef const& ref) noexcept {
    return ref.scale != 1.0 || ref.offset != 0.0;
}

} // namespace

BuiltinScalarCodec::BuiltinScalarCodec(dp::ScalarType type) : m_type(type) {}

std::string BuiltinScalarCodec::id() const { return idFor(m_type); }

std::string BuiltinScalarCodec::idFor(dp::ScalarType type) {
    std::string name = dp::scalarTypeName(type);
    for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return "builtin." + name;
}

dp::Value BuiltinScalarCodec::decode(core::RegisterWords const& raw,
                                     dp::PortRef const&     ref) {
    using dp::ScalarType;
    int const rc = dp::registerCountFor(m_type);
    if (rc > 0 && raw.size() < rc) {
        return {};
    }

    if (m_type == ScalarType::Bool) {
        int bit = ref.bit.value_or(0);
        return bool(((raw[0] >> bit) & 1u) != 0);
    }

    if (m_type == ScalarType::F32) {
        std::uint64_t const bits32 = unpackInt(raw, rc, ref.wordOrder);
        std::uint32_t const b      = std::uint32_t(bits32);
        float         f      = 0.0f;
        std::memcpy(&f, &b, sizeof(f));
        double        v      = double(f);
        if (hasLinearTransform(ref)) {
            v = v * ref.scale + ref.offset;
        }
        return v;
    }
    if (m_type == ScalarType::F64) {
        std::uint64_t const bits = unpackInt(raw, rc, ref.wordOrder);
        double        v    = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        if (hasLinearTransform(ref)) {
            v = v * ref.scale + ref.offset;
        }
        return v;
    }

    std::uint64_t const concat = unpackInt(raw, rc, ref.wordOrder);
    std::uint64_t const masked = (concat >> ref.shift) & ref.mask;

    auto integerVariant = [&](auto signedT, auto unsignedT) -> dp::Value {
        using S = decltype(signedT);
        using U = decltype(unsignedT);
        (void)signedT; (void)unsignedT;
        if (m_type == ScalarType::S16 || m_type == ScalarType::S32 ||
            m_type == ScalarType::S64) {
            S v = S(U(masked));
            if (hasLinearTransform(ref)) {
                return double(v) * ref.scale + ref.offset;
            }
            return std::int64_t(v);
        }
        U v = U(masked);
        if (hasLinearTransform(ref)) {
            return double(v) * ref.scale + ref.offset;
        }
        return std::uint64_t(v);
    };

    switch (m_type) {
        case ScalarType::U16:     return integerVariant(std::int16_t(0),  std::uint16_t(0));
        case ScalarType::S16:     return integerVariant(std::int16_t(0),  std::uint16_t(0));
        case ScalarType::U32:     return integerVariant(std::int32_t(0),  std::uint32_t(0));
        case ScalarType::S32:     return integerVariant(std::int32_t(0),  std::uint32_t(0));
        case ScalarType::U64:     return integerVariant(std::int64_t(0),  std::uint64_t(0));
        case ScalarType::S64:     return integerVariant(std::int64_t(0),  std::uint64_t(0));
        case ScalarType::EnumU16: return std::uint64_t(std::uint16_t(masked));   // wrapping codec maps it
        default:                  return {};
    }
}

core::RegisterWords BuiltinScalarCodec::encode(dp::Value const&    value,
                                            dp::PortRef const& ref) {
    using dp::ScalarType;
    int const rc = dp::registerCountFor(m_type);
    if (rc <= 0) return {};

    if (m_type == ScalarType::Bool) {
        core::RegisterWords out(rc, 0);
        if (dp::toBool(value)) {
            int bit = ref.bit.value_or(0);
            out[0]  = std::uint16_t(1u << bit);
        }
        return out;
    }

    if (m_type == ScalarType::F32) {
        double  v    = dp::toDouble(value);
        if (hasLinearTransform(ref)) v = (v - ref.offset) / ref.scale;
        float   f    = float(v);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(f));
        return packInt(bits, rc, ref.wordOrder);
    }
    if (m_type == ScalarType::F64) {
        double  v    = dp::toDouble(value);
        if (hasLinearTransform(ref)) v = (v - ref.offset) / ref.scale;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(v));
        return packInt(bits, rc, ref.wordOrder);
    }

    // Stay in 64-bit integer space when no linear transform is configured —
    // a double round-trip would lose precision above 2^53.
    std::int64_t raw = 0;
    if (hasLinearTransform(ref)) {
        double dval = (dp::toDouble(value) - ref.offset) / ref.scale;
        raw         = std::int64_t(std::llround(dval));
    } else if (m_type == ScalarType::U64) {
        raw = std::int64_t(dp::toUInt64(value));
    } else {
        raw = dp::toInt64(value);
    }
    std::uint64_t const masked  = std::uint64_t(raw) & ref.mask;
    std::uint64_t const shifted = masked << ref.shift;
    return packInt(shifted, rc, ref.wordOrder);
}

// ---------------------------------------------------------------------------
// EnumU16Codec
// ---------------------------------------------------------------------------

EnumU16Codec::EnumU16Codec(std::string                                      id,
                            std::unordered_map<std::uint16_t, std::string>   map)
    : m_id(std::move(id))
    , m_forward(std::move(map)) {
    m_reverse.reserve(m_forward.size());
    for (auto const& [raw, name] : m_forward) {
        m_reverse.emplace(name, raw);
    }
}

std::string EnumU16Codec::id() const { return m_id; }

dp::Value EnumU16Codec::decode(core::RegisterWords const& raw,
                               dp::PortRef const&     ref) {
    if (raw.empty()) return {};
    std::uint16_t const masked = std::uint16_t((raw[0] >> ref.shift) & std::uint16_t(ref.mask));
    auto it = m_forward.find(masked);
    if (it == m_forward.end()) {
        return std::string("Unknown(") + std::to_string(masked) + ")";
    }
    return it->second;
}

core::RegisterWords EnumU16Codec::encode(dp::Value const&    value,
                                     dp::PortRef const& ref) {
    std::string const name = dp::toString(value);
    std::uint16_t raw  = 0;
    if (auto it = m_reverse.find(name); it != m_reverse.end()) {
        raw = it->second;
    } else {
        raw = std::uint16_t(dp::toUInt64(value));
    }
    std::uint16_t shifted = std::uint16_t((raw & std::uint16_t(ref.mask)) << ref.shift);
    return {shifted};
}

} // namespace core::codec
