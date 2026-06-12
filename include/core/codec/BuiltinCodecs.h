// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/core_global.h"
#include "core/codec/Codec.h"
#include "core/dp/ScalarType.h"

namespace core::codec {

// BuiltinScalarCodec — pure-C++ codec covering every ScalarType except enum
// and string lookups. The decode pipeline (design §4.5) is:
//
//   1. WordOrder byte permutation (multi-register types only)
//   2. ScalarType byte → native value (incl. IEEE-754 reinterpretation)
//   3. shift / mask extraction (integer types only)
//   4. scale * raw + offset linear transform
//
// `encode` runs the same pipeline in reverse. Stateless; instances are safe
// to share between datapoints.
class CORE_EXPORT BuiltinScalarCodec : public Codec {
public:
    explicit BuiltinScalarCodec(dp::ScalarType type);

    std::string    id() const override;
    dp::Value       decode(core::RegisterWords const& raw,
                          dp::PortRef const&     ref) override;
    core::RegisterWords encode(dp::Value const&        value,
                          dp::PortRef const&     ref) override;

    dp::ScalarType scalarType() const noexcept { return m_type; }

    // Canonical builtin id for a given scalar type: "builtin.<scalar>".
    static std::string idFor(dp::ScalarType type);

private:
    dp::ScalarType m_type;
};

// EnumU16Codec — wraps a U16 scalar decode with a map<u16, string>. Missing
// keys decode to "Unknown(<n>)" so the datapoint always carries a value.
class CORE_EXPORT EnumU16Codec : public Codec {
public:
    EnumU16Codec(std::string id, std::unordered_map<std::uint16_t, std::string> map);

    std::string    id() const override;
    dp::Value       decode(core::RegisterWords const& raw,
                          dp::PortRef const&     ref) override;
    core::RegisterWords encode(dp::Value const&        value,
                          dp::PortRef const&     ref) override;

private:
    std::string                                      m_id;
    std::unordered_map<std::uint16_t, std::string>   m_forward;   // raw → name
    std::unordered_map<std::string, std::uint16_t>   m_reverse;   // name → raw
};

} // namespace core::codec
