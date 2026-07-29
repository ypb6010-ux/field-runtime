// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace wc {
class RuntimeHost;

// Conversion-rule CRUD and the engine that transforms a source datapoint into
// destination transport register writes.
void registerConversionControllers();
void startConversionEngine(RuntimeHost& runtime);
}
