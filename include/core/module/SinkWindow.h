// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/base/RegisterTable.h"
#include "core/core_global.h"
#include "core/module/FunctionalModule.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/TransportTypes.h"

namespace core::transport { class Transport; }

namespace core::module {

// SinkWindow — batched write target on one Transport's contiguous register
// range. Multiple Datapoints stage into a shared in-memory snapshot, and the
// window flushes the whole snapshot in a single Modbus write request when
// either debounce expires after a stage, the keep-alive period elapses, or
// `forceFlush()` is signalled (e.g. after a transport reconnect).
//
// Phase 2 ships the algorithm via `onTick()`; Phase 2.5 hooks a timer
// alongside PollRange.
class CORE_EXPORT SinkWindow : public FunctionalModule {
public:
    struct Config {
        std::string                   moduleId;
        core::RegisterTable           table        = core::RegisterTable::HoldingRegister;
        int                           startAddress = 0;
        int                           size         = 0;
        sched::Priority               priority     = sched::Priority::High;
        int                           debounceMs        = 20;
        int                           keepAlivePeriodMs = 0;   // 0 = disabled
        bool                          coalesceWrites    = true;
        core::RegisterWords                initial;
    };

    SinkWindow(Config cfg, transport::Transport& transport);
    ~SinkWindow() override;

    CORE_DISABLE_COPY_MOVE(SinkWindow)

    // Stage a register update into the in-memory snapshot. `absAddress` is
    // the absolute Modbus register address (i.e. the same address the
    // datapoint's sink PortRef carries). `mask` enables partial bit writes:
    //   snapshot[i] = (snapshot[i] & ~mask) | (value & mask)
    // Returns true when the staged value differed from the current snapshot.
    // Writes outside the window's range are silently dropped.
    bool stageRegister(int absAddress, std::uint16_t value, std::uint16_t mask = 0xFFFF);

    // After a transport reconnect, force the next tick to flush the whole
    // snapshot regardless of dirty / keep-alive timing.
    void forceFlush();

    // Drive the window's flush decision exactly once. Returns the scheduler
    // submission result when work was actually submitted, or an Ok with a
    // zero latency when no flush was due.
    sched::SubmitResult onTick();

    int                size()         const noexcept;
    int                startAddress() const noexcept;
    core::RegisterWords     snapshot()     const;
    bool               dirty()        const;

    // FunctionalModule
    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override;
    // Event-driven, non-blocking flush via the scheduler's async path.
    void driveTick()         override;

private:
    // Decide whether a flush is due; if so, snapshot the values + reason and
    // the staging generation at snapshot time. Returns false when nothing
    // needs writing. Shared by onTick / driveTick.
    bool decideFlush(core::RegisterWords& values, std::string& reason, std::uint64_t& gen);
    // On a successful write, clear dirty / forceFlush and stamp the flush time —
    // but ONLY if no new stage/forceFlush arrived since `gen` was snapshotted,
    // otherwise that update would be lost while the write was in flight.
    void markFlushed(bool ok, std::uint64_t gen);

    transport::Transport*              m_transport;
    Config                              m_cfg;
    core::RegisterWords                      m_snapshot;
    bool                                m_dirty       = false;
    bool                                m_forceFlush  = false;
    std::atomic<bool>                   m_started{false};
    // Bumped by every effective stageRegister() / forceFlush(); lets a flush
    // detect concurrent staging and avoid clearing dirty for data it did not
    // write (the in-flight lost-update race Codex flagged).
    std::uint64_t                       m_generation  = 0;
    std::chrono::steady_clock::time_point m_dirtyAt;
    std::chrono::steady_clock::time_point m_lastFlushAt;
    mutable std::mutex                   m_mtx;
    std::atomic<bool>                    m_inFlight{false};
    std::atomic<std::uint64_t>           m_runGeneration{0};
};

} // namespace core::module
