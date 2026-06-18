// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace wc {
class RuntimeHost;

// W6 hot-reload endpoints: builds a gateway TOML from the DB config and reloads
// the RuntimeHost on apply. `runtimeTomlPath` is where the generated config is
// written (the same file the RuntimeHost loads).
void registerConfigApply(RuntimeHost& runtime, std::string runtimeTomlPath);
}
