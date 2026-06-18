// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Registers the W2 configuration CRUD endpoints (transports / datapoints /
// codecs / poll_ranges) plus GET /api/v1/transports/kinds.
namespace wc {
void registerConfigControllers();
}
