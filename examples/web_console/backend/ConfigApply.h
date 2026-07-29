// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace wc {
class RuntimeHost;

// Builds gateway TOML from the database draft and transactionally reloads the
// RuntimeHost. `runtimeTomlPath` is the durable generated configuration.
void registerConfigApply(RuntimeHost& runtime, std::string runtimeTomlPath);
}
