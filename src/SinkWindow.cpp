// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/module/SinkWindow.h"

#include <algorithm>
#include <utility>

#include "core/transport/Transport.h"

namespace core::module {

using clock_t = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::duration_cast;

SinkWindow::SinkWindow(Config cfg, transport::Transport& transport)
    : m_transport(&transport)
    , m_cfg(std::move(cfg))
    , m_snapshot(m_cfg.size, 0) {
    m_id          = m_cfg.moduleId;
    m_transportId = transport.id();
    m_priority    = m_cfg.priority;
    int const populate = std::min(int(m_cfg.initial.size()), m_cfg.size);
    for (int i = 0; i < populate; ++i) {
        m_snapshot[i] = m_cfg.initial[i];
    }
}

SinkWindow::~SinkWindow() = default;

int            SinkWindow::size()         const noexcept { return m_cfg.size; }
int            SinkWindow::startAddress() const noexcept { return m_cfg.startAddress; }

int SinkWindow::tickPeriodMs() const {
    // Tick at the debounce cadence so a stage's debounce window closes within
    // one tick. A floor of 10 ms keeps us from monopolising the timer thread
    // when debounceMs is 0 or pathologically small.
    int const d = std::max(10, m_cfg.debounceMs);
    if (m_cfg.keepAlivePeriodMs > 0 && m_cfg.keepAlivePeriodMs < d) {
        return std::max(10, m_cfg.keepAlivePeriodMs);
    }
    return d;
}

QList<quint16> SinkWindow::snapshot() const {
    std::lock_guard lk(m_mtx);
    return m_snapshot;
}

bool SinkWindow::dirty() const {
    std::lock_guard lk(m_mtx);
    return m_dirty;
}

bool SinkWindow::stageRegister(int absAddress, quint16 value, quint16 mask) {
    int const idx = absAddress - m_cfg.startAddress;
    if (idx < 0 || idx >= m_cfg.size) return false;

    std::lock_guard lk(m_mtx);
    quint16 const cur  = m_snapshot[idx];
    quint16 const next = quint16((cur & ~mask) | (value & mask));
    if (next == cur) return false;
    m_snapshot[idx] = next;
    ++m_generation;   // an effective change — lost-update guard
    if (!m_dirty) {
        m_dirty   = true;
        m_dirtyAt = clock_t::now();
    }
    return true;
}

void SinkWindow::forceFlush() {
    std::lock_guard lk(m_mtx);
    m_forceFlush = true;
    ++m_generation;
}

bool SinkWindow::decideFlush(QList<quint16>& values, QString& reason, quint64& gen) {
    auto const now = clock_t::now();
    std::lock_guard lk(m_mtx);
    bool flush = false;
    if (m_forceFlush) {
        flush  = true;
        reason = QStringLiteral("force");
    } else if (m_dirty) {
        auto const elapsed = duration_cast<milliseconds>(now - m_dirtyAt).count();
        if (elapsed >= m_cfg.debounceMs) {
            flush  = true;
            reason = QStringLiteral("debounce");
        }
    }
    if (!flush && m_cfg.keepAlivePeriodMs > 0) {
        auto const elapsed = duration_cast<milliseconds>(now - m_lastFlushAt).count();
        if (elapsed >= m_cfg.keepAlivePeriodMs) {
            flush  = true;
            reason = QStringLiteral("keepalive");
        }
    }
    if (!flush) return false;
    values = m_snapshot;
    gen    = m_generation;
    return true;
}

void SinkWindow::markFlushed(bool ok, quint64 gen) {
    if (!ok) return;   // dirty preserved on failure so the next tick retries
    std::lock_guard lk(m_mtx);
    // If a stage / forceFlush landed since the snapshot, that data was not in
    // this write — keep dirty so the next tick flushes it (no lost update).
    if (m_generation != gen) return;
    m_dirty       = false;
    m_forceFlush  = false;
    m_lastFlushAt = clock_t::now();
}

sched::SubmitResult SinkWindow::onTick() {
    if (!m_started.load()) {
        return {sched::ResultKind::Cancelled,
                QStringLiteral("module not started"), 0};
    }

    QList<quint16> values;
    QString        reason;
    quint64        gen = 0;
    if (!decideFlush(values, reason, gen)) {
        return {sched::ResultKind::Ok, QStringLiteral("no flush due"), 0};
    }

    transport::WriteBatch batch;
    batch.table        = m_cfg.table;
    batch.startAddress = m_cfg.startAddress;
    batch.values       = std::move(values);

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;

    transport::WriteResult write{};
    auto submission = m_transport->scheduler().submit(tag, [&] {
        write = m_transport->writeBatch(batch);
    });

    if (submission.kind != sched::ResultKind::Ok) {
        return submission;
    }

    markFlushed(write.ok, gen);

    if (!write.ok) {
        submission.kind         = sched::ResultKind::Error;
        submission.errorMessage = write.errorMessage;
    }
    submission.errorMessage = reason + (write.ok ? QString{} : (": " + write.errorMessage));
    return submission;
}

void SinkWindow::driveTick() {
    if (!m_started.load()) return;

    bool expected = false;
    if (!m_inFlight.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
        return;
    }

    QList<quint16> values;
    QString        reason;
    quint64        gen = 0;
    if (!decideFlush(values, reason, gen)) {
        m_inFlight.store(false, std::memory_order_release);
        return;
    }

    transport::WriteBatch batch;
    batch.table        = m_cfg.table;
    batch.startAddress = m_cfg.startAddress;
    batch.values       = std::move(values);

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;

    auto const submission = m_transport->scheduler().submitAsync(tag,
        [this, batch, gen](sched::AsyncDone done) {
            m_transport->writeAsync(batch,
                [this, gen, done = std::move(done)](transport::WriteResult w) mutable {
                    markFlushed(w.ok, gen);
                    m_inFlight.store(false, std::memory_order_release);
                    done(w.ok);
                });
        });

    if (submission.kind != sched::ResultKind::Ok) {
        m_inFlight.store(false, std::memory_order_release);
    }
}

void SinkWindow::start() {
    std::lock_guard lk(m_mtx);
    m_started     = true;
    // Anchor lastFlushAt so the keep-alive period measures from start rather
    // than the epoch (which would trigger a spurious immediate flush).
    m_lastFlushAt = clock_t::now();
}

void SinkWindow::stop() {
    std::lock_guard lk(m_mtx);
    m_started = false;
}

void SinkWindow::pause()  { stop(); }
void SinkWindow::resume() { start(); }

} // namespace core::module
