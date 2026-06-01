#include "core/module/Heartbeat.h"

#include <utility>

#include "core/transport/Transport.h"

namespace core::module {

using clock_t = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

Heartbeat::Heartbeat(Config cfg, transport::Transport& transport)
    : m_transport(&transport)
    , m_cfg(std::move(cfg)) {
    m_id          = m_cfg.moduleId;
    m_transportId = transport.id();
    m_priority    = m_cfg.priority;
}

Heartbeat::~Heartbeat() = default;

int Heartbeat::periodMs() const noexcept { return m_cfg.periodMs; }

sched::SubmitResult Heartbeat::onTick() {
    if (!m_started) {
        return {sched::ResultKind::Cancelled,
                QStringLiteral("module not started"), 0};
    }
    auto const now = clock_t::now();
    auto const elapsed = duration_cast<milliseconds>(now - m_lastSentAt).count();
    if (m_cfg.periodMs > 0 && elapsed < m_cfg.periodMs) {
        return {sched::ResultKind::Ok, QStringLiteral("not yet"), 0};
    }

    transport::WriteBatch batch;
    batch.table        = m_cfg.table;
    batch.startAddress = m_cfg.address;
    batch.values       = m_cfg.values;

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;

    transport::WriteResult write{};
    auto submission = m_transport->scheduler().submit(tag, [&] {
        write = m_transport->writeBatch(batch);
    });
    if (submission.kind == sched::ResultKind::Ok) {
        m_lastSentAt = now;
        if (!write.ok) {
            submission.kind         = sched::ResultKind::Error;
            submission.errorMessage = write.errorMessage;
        }
    }
    return submission;
}

void Heartbeat::start() {
    m_started    = true;
    // Anchor so the first onTick fires immediately if periodMs > 0 would
    // otherwise consider us "too soon": setting it back makes the first
    // elapsed measurement large enough to trigger.
    m_lastSentAt = clock_t::time_point{};
}

void Heartbeat::stop()   { m_started = false; }
void Heartbeat::pause()  { m_started = false; }
void Heartbeat::resume() { m_started = true; }

} // namespace core::module
