#pragma once

#include "core/core_global.h"

namespace core {

class ICore;

namespace internal {

// Drive every PollRange registered with the core through exactly one tick.
// Production code uses a QTimer wired by ModuleRegistry::startAll();
// tests poke this entry point to keep their orchestration deterministic.
CORE_EXPORT void pollAllOnce(ICore& core);

// Drive every SinkWindow's onTick() so tests can deterministically force a
// flush evaluation without spinning the QTimer loop.
CORE_EXPORT void tickSinkWindowsOnce(ICore& core);

} // namespace internal
} // namespace core
