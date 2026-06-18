// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/base/RegisterTable.h"

namespace core::gateway {

// Transports that cache the raw holding-register words they last polled expose
// them through this interface so bridge mirroring (mirrorBridgesOnce) can copy
// RAW source registers without knowing the concrete transport type. Unknown
// addresses read back as 0.
class RegisterSnapshotSource {
public:
    virtual ~RegisterSnapshotSource() = default;
    virtual core::RegisterWords snapshotHoldingRegisters(int start, int count) const = 0;
};

} // namespace core::gateway
