#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include <QList>
#include <QString>

#include "core/sched/SerialScheduler.h"
#include "core/transport/Transport.h"

namespace core::test {

// Header-only MockTransport for unit tests. Owns its own SerialScheduler so
// the half-duplex serialisation guarantees are part of every test that wires
// through it.
class MockTransport : public core::transport::Transport {
public:
    using ConnectionState = core::transport::ConnectionState;
    using ReadRequest     = core::transport::ReadRequest;
    using WriteBatch      = core::transport::WriteBatch;
    using ReadResult      = core::transport::ReadResult;
    using WriteResult     = core::transport::WriteResult;

    explicit MockTransport(QString id = QStringLiteral("mock"))
        : m_id(std::move(id))
        , m_scheduler(std::make_unique<core::sched::SerialScheduler>(
              core::sched::SchedulerConfig{})) {}

    // ─── core::transport::Transport ────────────────────────────────────
    QString               id()    const override { return m_id; }
    core::transport::TransportKind kind() const override {
        return core::transport::TransportKind::ModbusTcpClient;
    }
    ConnectionState       state() const override {
        std::lock_guard lk(m_mtx);
        return m_state;
    }
    std::expected<void, QString> connect() override {
        std::lock_guard lk(m_mtx);
        m_state = ConnectionState::Connected;
        return {};
    }
    void disconnect() override {
        std::lock_guard lk(m_mtx);
        m_state = ConnectionState::Disconnected;
    }

    core::sched::RequestScheduler& scheduler() override { return *m_scheduler; }

    ReadResult read(ReadRequest const& req) override {
        std::lock_guard lk(m_mtx);
        m_reads.append(req);
        if (m_readResponses.empty()) {
            // Default: ok-with-zeros sized to the request count so default
            // PollRange wiring sees a usable response.
            ReadResult r;
            r.ok           = true;
            r.startAddress = req.startAddress;
            r.values       = QList<quint16>(req.count, 0);
            return r;
        }
        auto r = m_readResponses.front();
        m_readResponses.pop_front();
        if (r.startAddress == 0 && r.ok) r.startAddress = req.startAddress;
        return r;
    }

    WriteResult writeBatch(WriteBatch const& batch) override {
        std::lock_guard lk(m_mtx);
        m_writes.append(batch);
        if (m_writeResponses.empty()) {
            return {true, {}};
        }
        auto r = m_writeResponses.front();
        m_writeResponses.pop_front();
        return r;
    }

    // ─── Test hooks ────────────────────────────────────────────────────
    void enqueueReadResult(ReadResult r) {
        std::lock_guard lk(m_mtx);
        m_readResponses.push_back(std::move(r));
    }
    void enqueueReadValues(QList<quint16> values) {
        ReadResult r;
        r.ok     = true;
        r.values = std::move(values);
        enqueueReadResult(std::move(r));
    }
    void enqueueReadError(QString msg) {
        ReadResult r;
        r.ok           = false;
        r.errorMessage = std::move(msg);
        enqueueReadResult(std::move(r));
    }
    void enqueueWriteResult(WriteResult r) {
        std::lock_guard lk(m_mtx);
        m_writeResponses.push_back(std::move(r));
    }

    QList<ReadRequest> capturedReads() const {
        std::lock_guard lk(m_mtx);
        return m_reads;
    }
    QList<WriteBatch> capturedWrites() const {
        std::lock_guard lk(m_mtx);
        return m_writes;
    }
    int readCount() const {
        std::lock_guard lk(m_mtx);
        return m_reads.size();
    }

private:
    QString                                                  m_id;
    std::unique_ptr<core::sched::SerialScheduler>            m_scheduler;
    mutable std::mutex                                       m_mtx;
    ConnectionState                                          m_state = ConnectionState::Connected;
    std::deque<ReadResult>                                   m_readResponses;
    std::deque<WriteResult>                                  m_writeResponses;
    QList<ReadRequest>                                       m_reads;
    QList<WriteBatch>                                        m_writes;
};

} // namespace core::test
