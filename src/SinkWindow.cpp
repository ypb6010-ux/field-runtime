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
    if (!m_dirty) {
        m_dirty   = true;
        m_dirtyAt = clock_t::now();
    }
    return true;
}

void SinkWindow::forceFlush() {
    std::lock_guard lk(m_mtx);
    m_forceFlush = true;
}

sched::SubmitResult SinkWindow::onTick() {
    if (!m_started) {
        return {sched::ResultKind::Cancelled,
                QStringLiteral("module not started"), 0};
    }

    auto const  now = clock_t::now();
    bool        flush = false;
    QString     reason;
    QList<quint16> values;

    {
        std::lock_guard lk(m_mtx);
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
        if (!flush) {
            return {sched::ResultKind::Ok, QStringLiteral("no flush due"), 0};
        }
        values = m_snapshot;
    }

    transport::WriteBatch batch;
    batch.table        = m_cfg.table;
    batch.startAddress = m_cfg.startAddress;
    batch.values       = std::move(values);

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;
    tag.coalesce = m_cfg.coalesceWrites;

    transport::WriteResult write{};
    auto submission = m_transport->scheduler().submit(tag, [&] {
        write = m_transport->writeBatch(batch);
    });

    if (submission.kind != sched::ResultKind::Ok) {
        return submission;
    }

    {
        std::lock_guard lk(m_mtx);
        if (write.ok) {
            // Clear dirty / forceFlush; record successful flush time. Dirty
            // is intentionally preserved when writes fail so the next tick
            // automatically retries.
            m_dirty       = false;
            m_forceFlush  = false;
            m_lastFlushAt = now;
        }
    }

    if (!write.ok) {
        submission.kind         = sched::ResultKind::Error;
        submission.errorMessage = write.errorMessage;
    }
    submission.errorMessage = reason + (write.ok ? QString{} : (": " + write.errorMessage));
    return submission;
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
