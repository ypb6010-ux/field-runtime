// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace wc {
class RuntimeHost;

// W7 protocol conversion: CRUD for conversion_rules + a periodic engine that
// reads a source datapoint, applies a transform, and writes the result to a
// destination transport register via the RuntimeHost.
void registerConversionControllers();
void startConversionEngine(RuntimeHost& runtime);
}
