#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <QList>
#include <QString>
#include <QtSerialBus/QModbusDataUnit>

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
// Phase 2 ships the algorithm via `onTick()`; Phase 2.5 hooks a QTimer
// alongside PollRange.
class CORE_EXPORT SinkWindow : public FunctionalModule {
public:
    struct Config {
        QString                       moduleId;
        QModbusDataUnit::RegisterType table        = QModbusDataUnit::HoldingRegisters;
        int                           startAddress = 0;
        int                           size         = 0;
        sched::Priority               priority     = sched::Priority::High;
        int                           debounceMs        = 20;
        int                           keepAlivePeriodMs = 0;   // 0 = disabled
        bool                          coalesceWrites    = true;
        QList<quint16>                initial;
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
    bool stageRegister(int absAddress, quint16 value, quint16 mask = 0xFFFF);

    // After a transport reconnect, force the next tick to flush the whole
    // snapshot regardless of dirty / keep-alive timing.
    void forceFlush();

    // Drive the window's flush decision exactly once. Returns the scheduler
    // submission result when work was actually submitted, or an Ok with a
    // zero latency when no flush was due.
    sched::SubmitResult onTick();

    int                size()         const noexcept;
    int                startAddress() const noexcept;
    QList<quint16>     snapshot()     const;
    bool               dirty()        const;

    // FunctionalModule
    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override;
    void driveTick()         override { (void)onTick(); }

private:
    transport::Transport*              m_transport;
    Config                              m_cfg;
    QList<quint16>                      m_snapshot;
    bool                                m_dirty       = false;
    bool                                m_forceFlush  = false;
    bool                                m_started     = false;
    std::chrono::steady_clock::time_point m_dirtyAt;
    std::chrono::steady_clock::time_point m_lastFlushAt;
    mutable std::mutex                   m_mtx;
};

} // namespace core::module
