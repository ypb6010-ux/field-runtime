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

// Drive one round of `SchedulerStatsEvent` publication for every transport.
// Tests use this to verify scheduler-stats wiring without depending on the
// QTimer-driven pump.
CORE_EXPORT void publishSchedulerStatsOnce(ICore& core);

// Drive one round of [[bridge]] PLC→server mirroring so tests can assert the
// server's register table without waiting on the QTimer-driven mirror pump.
CORE_EXPORT void mirrorBridgesOnce(ICore& core);

} // namespace internal
} // namespace core
