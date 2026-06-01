#pragma once

#include "core_global.h"

namespace core {

constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

CORE_EXPORT const char* versionString() noexcept;

} // namespace core
