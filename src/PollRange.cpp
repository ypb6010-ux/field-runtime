// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/module/PollRange.h"

#include <utility>

#include "core/codec/Codec.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/transport/Transport.h"

namespace core::module {

PollRange::PollRange(QString                     moduleId,
                     transport::Transport&       transport,
                     transport::ReadRequest      request,
                     int                         periodMs,
                     sched::Priority             priority,
                     bus::EventBus*               bus)
    : m_transport(&transport)
    , m_req(std::move(request))
    , m_periodMs(periodMs)
    , m_bus(bus) {
    m_id          = std::move(moduleId);
    m_transportId = transport.id();
    m_priority    = priority;
}

PollRange::~PollRange() = default;

void PollRange::bind(std::shared_ptr<dp::Datapoint> datapoint,
                     std::shared_ptr<codec::Codec>  codec,
                     int                             registerOffset) {
    if (!datapoint || !codec) return;
    auto source = datapoint->source();
    int const registerCount = dp::registerCountFor(datapoint->type());
    m_bindings.push_back(Binding{std::move(datapoint),
                                  std::move(codec),
                                  std::move(source),
                                  registerOffset,
                                  registerCount});
}

int PollRange::periodMs()    const noexcept { return m_periodMs; }
int PollRange::bindingCount() const noexcept { return m_bindings.size(); }

transport::ReadRequest const& PollRange::request() const noexcept {
    return m_req;
}

void PollRange::applyResult(transport::ReadResult const& result) {
    if (!result.ok || result.values.size() < m_req.count) {
        for (auto& b : m_bindings) {
            b.dp->setState(dp::DpState::Error);
        }
        return;
    }
    // Successful read: dispatch slices to each bound datapoint.
    for (auto& b : m_bindings) {
        int const rc = b.registerCount;
        if (rc <= 0 || b.offset < 0 || b.offset + rc > result.values.size()) {
            b.dp->setState(dp::DpState::Error);
            continue;
        }
        auto const sub = result.values.mid(b.offset, rc);
        // The codec needs a PortRef for word-order / mask / scale / bit
        // metadata. We expect the datapoint's `source` to carry it; if a
        // datapoint somehow ended up here without a source the binding is
        // mis-wired — mark Error rather than silently producing garbage.
        if (!b.source.has_value()) {
            b.dp->setState(dp::DpState::Error);
            continue;
        }
        QVariant decoded;
        try {
            decoded = b.codec->decode(sub, *b.source);
        } catch (...) {
            b.dp->setState(dp::DpState::Error);
            continue;
        }
        if (!decoded.isValid()) {
            b.dp->setState(dp::DpState::Error);
            continue;
        }
        b.dp->setValue(std::move(decoded));
    }
    if (m_bus) {
        m_bus->publish(bus::PollRangeCompleted{
            m_id, m_transportId, m_req.table, m_req.startAddress, m_req.count,
            result.values.mid(0, m_req.count), QDateTime::currentDateTimeUtc()});
    }
}

sched::SubmitResult PollRange::pollOnce() {
    if (m_paused.load()) {
        return {sched::ResultKind::Cancelled,
                QStringLiteral("module paused"), 0};
    }

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;

    transport::ReadResult result{};
    auto submission = m_transport->scheduler().submit(tag, [&] {
        result = m_transport->read(m_req);
    });

    // Even if the scheduler accepted the work, the underlying I/O may have
    // failed. We translate both layers into datapoint state so QML and
    // database consumers see a consistent picture.
    if (submission.kind != sched::ResultKind::Ok) {
        for (auto& b : m_bindings) {
            b.dp->setState(dp::DpState::Stale);
        }
        return submission;
    }
    applyResult(result);
    if (!result.ok || result.values.size() < m_req.count) {
        submission.kind         = sched::ResultKind::Error;
        submission.errorMessage = result.errorMessage.isEmpty()
            ? QStringLiteral("incomplete read result") : result.errorMessage;
    }
    return submission;
}

void PollRange::driveTick() {
    if (m_paused.load()) return;

    // Coalesce: if the previous poll has not finished (slow / unreachable PLC),
    // skip this tick instead of stacking another read in the queue.
    bool expected = false;
    if (!m_inFlight.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
        return;
    }

    sched::RequestTag tag;
    tag.moduleId = m_id;
    tag.priority = m_priority;
    quint64 const runGeneration = m_runGeneration.load(std::memory_order_acquire);

    auto const submission = m_transport->scheduler().submitAsync(tag,
        [this, runGeneration](sched::AsyncDone done) {
            m_transport->readAsync(m_req,
                [this, runGeneration, done = std::move(done)](transport::ReadResult r) mutable {
                    bool const successful = r.ok && r.values.size() >= m_req.count;
                    if (!m_paused.load(std::memory_order_acquire)
                        && m_runGeneration.load(std::memory_order_acquire)
                            == runGeneration) {
                        applyResult(r);
                    }
                    m_inFlight.store(false, std::memory_order_release);
                    done(successful);
                });
        });

    if (submission.kind != sched::ResultKind::Ok) {
        // Rejected by the scheduler (circuit open / queue full): mark stale and
        // release the coalesce guard so the next tick can retry.
        if (!m_paused.load(std::memory_order_acquire)
            && m_runGeneration.load(std::memory_order_acquire) == runGeneration) {
            for (auto& b : m_bindings) {
                b.dp->setState(dp::DpState::Stale);
            }
        }
        m_inFlight.store(false, std::memory_order_release);
    }
}

void PollRange::start()  {
    m_runGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_paused.store(false, std::memory_order_release);
}
void PollRange::stop()   {
    m_paused.store(true, std::memory_order_release);
    m_runGeneration.fetch_add(1, std::memory_order_acq_rel);
}
void PollRange::pause()  {
    m_paused.store(true, std::memory_order_release);
    m_runGeneration.fetch_add(1, std::memory_order_acq_rel);
}
void PollRange::resume() {
    m_runGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_paused.store(false, std::memory_order_release);
}

} // namespace core::module
