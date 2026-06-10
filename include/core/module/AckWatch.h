// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"

namespace core::bus { class EventBus; }

namespace core::module {

// AckWatch — blocks the caller until a `bus::DpChanged` event matching
// (dpId, expected) is published, or the configured timeout elapses. Used by
// the UntilAck command pattern: stage a write, then waitOnce() for the
// expected feedback state, then stage the clear-out.
//
// Phase 2 ships the synchronous primitive on std::condition_variable; a
// coroutine variant lands when EventBus::waitFor is implemented.
class CORE_EXPORT AckWatch : public FunctionalModule {
public:
    enum class AckResult {
        Ok,
        Timeout,
        Cancelled,
    };

    struct Config {
        QString  moduleId;
        QString  dpId;
        QVariant expected;
        int      timeoutMs = 3000;
    };

    AckWatch(Config cfg, bus::EventBus& bus);
    ~AckWatch() override;

    CORE_DISABLE_COPY_MOVE(AckWatch)

    AckResult waitOnce();
    void      cancel();

    QString dpId() const;

    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override { return 0; }
    void driveTick()         override {}

private:
    class Impl;
    Impl* m_impl;
};

} // namespace core::module
