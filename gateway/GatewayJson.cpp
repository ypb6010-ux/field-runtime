// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "GatewayJson.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <type_traits>
#include <variant>

namespace core::gateway::json {

void appendString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::ostringstream os;
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
                    out += os.str();
                } else {
                    out.push_back(char(c));
                }
                break;
        }
    }
    out.push_back('"');
}

std::string string(std::string_view value) {
    std::string out;
    appendString(out, value);
    return out;
}

std::string value(dp::Value const& value) {
    return std::visit([](auto const& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            return x ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return string(x);
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(x)) return "null";
            std::ostringstream os;
            os << std::setprecision(17) << x;
            return os.str();
        } else {
            return std::to_string(x);
        }
    }, value);
}

std::string dpState(dp::DpState state) {
    switch (state) {
        case dp::DpState::Ok: return "Ok";
        case dp::DpState::Stale: return "Stale";
        case dp::DpState::Error: return "Error";
        case dp::DpState::Missing: return "Missing";
    }
    return "Unknown";
}

std::int64_t timestampMs(dp::Timestamp ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        ts.time_since_epoch()).count();
}

} // namespace core::gateway::json
