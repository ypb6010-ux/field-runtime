#pragma once

#include "core_global.h"

namespace core {

constexpr int kVersionMajor = 2;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 0;

CORE_EXPORT const char* versionString() noexcept;

} // namespace core
