// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace wc {
class RuntimeHost;
// Starts the periodic push pump that streams live datapoints / transport states
// to subscribers of the /ws/stream WebSocket.
void startWsPump(RuntimeHost& runtime);
}
