// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace wc {
class RuntimeHost;

// W4 data endpoints: GET /data/history, POST /data/write.
void registerDataControllers(RuntimeHost& runtime);
// Periodic history sampler: snapshots live datapoints into the `samples` table.
void startSampler(RuntimeHost& runtime);
}
