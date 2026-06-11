// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/core_global.h"

namespace core::dp {

// All scalar types representable as a datapoint value. Multi-register types
// (U32/S32/F32 and 64-bit variants) require an explicit WordOrder.
enum class ScalarType {
    Bool,
    U16,
    S16,
    U32,
    S32,
    F32,
    U64,
    S64,
    F64,
    EnumU16,
    String,
};

CORE_EXPORT int registerCountFor(ScalarType type) noexcept;  // 1, 2, 4 ...
CORE_EXPORT bool isMultiRegister(ScalarType type) noexcept;
CORE_EXPORT const char* scalarTypeName(ScalarType type) noexcept;

} // namespace core::dp
