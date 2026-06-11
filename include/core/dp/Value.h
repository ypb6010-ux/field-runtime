// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// --- core-base ---------------------------------------------------------------
// Qt-free datapoint value. Replaces QVariant in the value model (codec
// decode/encode, Datapoint storage, routing) so the abstraction carries no
// QtCore dynamic type. The QML boundary marshals Value <-> QVariant via the
// Qt-side bridge include/core/dp/ValueQt.h.
//
// Coercion helpers preserve the permissive QVariant-style conversions the
// codecs relied on (e.g. a value written from QML as a JS number arriving as
// double but encoded into an integer register).

#include <cstdint>
#include <string>
#include <variant>

namespace core::dp {

using Value = std::variant<std::monostate, bool,
                           std::int64_t, std::uint64_t,
                           double, std::string>;

inline bool isNull(Value const& v) {
    return std::holds_alternative<std::monostate>(v);
}

inline bool toBool(Value const& v) {
    return std::visit([](auto const& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return false;
        else if constexpr (std::is_same_v<T, std::string>) return !x.empty() && x != "0" && x != "false";
        else if constexpr (std::is_same_v<T, bool>) return x;
        else return x != T{};
    }, v);
}

inline double toDouble(Value const& v) {
    return std::visit([](auto const& x) -> double {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return 0.0;
        else if constexpr (std::is_same_v<T, std::string>) {
            try { return std::stod(x); } catch (...) { return 0.0; }
        }
        else if constexpr (std::is_same_v<T, bool>) return x ? 1.0 : 0.0;
        else return double(x);
    }, v);
}

inline std::int64_t toInt64(Value const& v) {
    return std::visit([](auto const& x) -> std::int64_t {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return 0;
        else if constexpr (std::is_same_v<T, std::string>) {
            try { return std::int64_t(std::stoll(x)); } catch (...) { return 0; }
        }
        else if constexpr (std::is_same_v<T, bool>) return x ? 1 : 0;
        else if constexpr (std::is_same_v<T, double>) return std::int64_t(x);
        else return std::int64_t(x);
    }, v);
}

inline std::uint64_t toUInt64(Value const& v) {
    return std::visit([](auto const& x) -> std::uint64_t {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return 0;
        else if constexpr (std::is_same_v<T, std::string>) {
            try { return std::uint64_t(std::stoull(x)); } catch (...) { return 0; }
        }
        else if constexpr (std::is_same_v<T, bool>) return x ? 1 : 0;
        else if constexpr (std::is_same_v<T, double>) return std::uint64_t(x);
        else return std::uint64_t(x);
    }, v);
}

inline std::string toString(Value const& v) {
    return std::visit([](auto const& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return {};
        else if constexpr (std::is_same_v<T, std::string>) return x;
        else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else return std::to_string(x);
    }, v);
}

} // namespace core::dp
