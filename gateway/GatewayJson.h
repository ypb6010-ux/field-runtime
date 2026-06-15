// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/dp/State.h"
#include "core/dp/Value.h"

namespace core::gateway::json {

void appendString(std::string& out, std::string_view value);
std::string string(std::string_view value);
std::string value(dp::Value const& value);
std::string dpState(dp::DpState state);
std::int64_t timestampMs(dp::Timestamp ts);

} // namespace core::gateway::json
