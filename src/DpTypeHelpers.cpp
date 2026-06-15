// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// Qt-free helpers for the datapoint scalar/word-order enums. Lives in
// FieldRuntimeBase (and Core) so every consumer — including the without-Qt
// gateway — links one canonical definition. (Previously these were inlined in
// the Qt-side Stubs.cpp, which left FieldRuntimeBase referencing undefined
// symbols.)

#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"

namespace core::dp {

// ---------------------------------------------------------------------------
// ScalarType helpers
// ---------------------------------------------------------------------------
int registerCountFor(ScalarType type) noexcept {
    switch (type) {
        case ScalarType::Bool:
        case ScalarType::U16:
        case ScalarType::S16:
        case ScalarType::EnumU16: return 1;
        case ScalarType::U32:
        case ScalarType::S32:
        case ScalarType::F32:     return 2;
        case ScalarType::U64:
        case ScalarType::S64:
        case ScalarType::F64:     return 4;
        case ScalarType::String:  return 0;   // variable length
    }
    return 0;
}

bool isMultiRegister(ScalarType type) noexcept {
    return registerCountFor(type) > 1;
}

const char* scalarTypeName(ScalarType type) noexcept {
    switch (type) {
        case ScalarType::Bool:    return "Bool";
        case ScalarType::U16:     return "U16";
        case ScalarType::S16:     return "S16";
        case ScalarType::U32:     return "U32";
        case ScalarType::S32:     return "S32";
        case ScalarType::F32:     return "F32";
        case ScalarType::U64:     return "U64";
        case ScalarType::S64:     return "S64";
        case ScalarType::F64:     return "F64";
        case ScalarType::EnumU16: return "EnumU16";
        case ScalarType::String:  return "String";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// WordOrder helpers
// ---------------------------------------------------------------------------
const char* wordOrderName(WordOrder w) noexcept {
    switch (w) {
        case WordOrder::ABCD: return "ABCD";
        case WordOrder::CDAB: return "CDAB";
        case WordOrder::BADC: return "BADC";
        case WordOrder::DCBA: return "DCBA";
    }
    return "?";
}

BytePermutation permutationFor(WordOrder w, int byteCount) noexcept {
    BytePermutation p{};
    if (byteCount == 4) {
        switch (w) {
            case WordOrder::ABCD: p.order = {0,1,2,3,0,0,0,0}; break;
            case WordOrder::CDAB: p.order = {2,3,0,1,0,0,0,0}; break;
            case WordOrder::BADC: p.order = {1,0,3,2,0,0,0,0}; break;
            case WordOrder::DCBA: p.order = {3,2,1,0,0,0,0,0}; break;
        }
    } else if (byteCount == 8) {
        switch (w) {
            case WordOrder::ABCD: p.order = {0,1,2,3,4,5,6,7}; break;
            case WordOrder::CDAB: p.order = {6,7,4,5,2,3,0,1}; break;
            case WordOrder::BADC: p.order = {1,0,3,2,5,4,7,6}; break;
            case WordOrder::DCBA: p.order = {7,6,5,4,3,2,1,0}; break;
        }
    }
    return p;
}

} // namespace core::dp
