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

    // ─── Async I/O ─────────────────────────────────────────────────────
    // By default async completes synchronously (base Transport default). In
    // "defer" mode the completion is parked so a test can hold a request
    // in-flight and fire it on demand — needed to exercise coalescing.
    void setDeferAsync(bool on) {
        std::lock_guard lk(m_mtx);
        m_deferAsync = on;
    }

    void readAsync(ReadRequest const& req, ReadDone done) override {
        bool defer;
        { std::lock_guard lk(m_mtx); defer = m_deferAsync; }
        if (!defer) { Transport::readAsync(req, std::move(done)); return; }
        ReadResult r = read(req);   // records the read + pops the response now
        std::lock_guard lk(m_mtx);
        m_pendingReads.push_back({std::move(r), std::move(done)});
    }

    void writeAsync(WriteBatch const& batch, WriteDone done) override {
        bool defer;
        { std::lock_guard lk(m_mtx); defer = m_deferAsync; }
        if (!defer) { Transport::writeAsync(batch, std::move(done)); return; }
        WriteResult r = writeBatch(batch);
        std::lock_guard lk(m_mtx);
        m_pendingWrites.push_back({std::move(r), std::move(done)});
    }

    bool completeNextRead() {
        std::pair<ReadResult, ReadDone> p;
        {
            std::lock_guard lk(m_mtx);
            if (m_pendingReads.empty()) return false;
            p = std::move(m_pendingReads.front());
            m_pendingReads.pop_front();
        }
        p.second(std::move(p.first));   // fire completion outside the lock
        return true;
    }
    bool completeNextWrite() {
        std::pair<WriteResult, WriteDone> p;
        {
            std::lock_guard lk(m_mtx);
            if (m_pendingWrites.empty()) return false;
            p = std::move(m_pendingWrites.front());
            m_pendingWrites.pop_front();
        }
        p.second(std::move(p.first));
        return true;
    }
    int pendingReadCount() const {
        std::lock_guard lk(m_mtx);
        return int(m_pendingReads.size());
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
    bool                                                     m_deferAsync = false;
    std::deque<std::pair<ReadResult, ReadDone>>              m_pendingReads;
    std::deque<std::pair<WriteResult, WriteDone>>            m_pendingWrites;
};

} // namespace core::test
