// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/OpcUaClientTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <QMetaObject>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#ifdef CORE_HAS_OPCUA
  #include <QtOpcUa/QOpcUaAuthenticationInformation>
  #include <QtOpcUa/QOpcUaClient>
  #include <QtOpcUa/QOpcUaEndpointDescription>
  #include <QtOpcUa/QOpcUaNode>
  #include <QtOpcUa/QOpcUaProvider>
#endif

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

#include "QtThreadInvoke.h"
#include "TransportStatusTracker.h"

namespace core::transport {

#ifdef CORE_HAS_OPCUA

namespace {

ConnectionState stateFromQt(QOpcUaClient::ClientState s) {
    switch (s) {
        case QOpcUaClient::Disconnected: return ConnectionState::Disconnected;
        case QOpcUaClient::Connecting:   return ConnectionState::Connecting;
        case QOpcUaClient::Connected:    return ConnectionState::Connected;
        case QOpcUaClient::Closing:      return ConnectionState::Disconnected;
    }
    return ConnectionState::Disconnected;
}

struct PendingNodeRead {
    QSemaphore done{0};
    QVariant value;
    bool ok = false;
};

struct PendingNodeWrite {
    QSemaphore done{0};
    bool ok = false;
};

} // namespace

class OpcUaClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , statusTracker(cfg.id, TransportKind::OpcUaClient, b, {},
                        EndpointInfo{QUrl(cfg.endpointUrl).host(),
                                     quint16(QUrl(cfg.endpointUrl).port(4840))})
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread) {

        // Create the OPC UA client on the caller thread (provider plugins
        // load synchronously), then ship it over to the worker so all
        // subsequent network I/O runs there. invokeMethod-on-QThread from
        // the same thread deadlocks, so we never use that pattern.
        QOpcUaProvider provider;
        client = provider.createClient(cfg.backend);
        if (!client) {
            auto const message = QStringLiteral("provider failed to create '%1' backend")
                                     .arg(cfg.backend);
            setLastError(message);
            state.store(ConnectionState::Error, std::memory_order_release);
            statusTracker.update(ConnectionState::Error, message);
            thread->start();   // keep the worker thread alive for symmetry
            return;
        }
        client->setApplicationIdentity({});
        QObject::connect(client, &QOpcUaClient::stateChanged,
                         client, [this](QOpcUaClient::ClientState s) {
            auto const cur  = stateFromQt(s);
            if (cur == ConnectionState::Connected) setLastError({});
            auto const error = getLastError();
            auto const effective = cur == ConnectionState::Disconnected
                                && !error.isEmpty()
                ? ConnectionState::Error : cur;
            state.store(effective, std::memory_order_release);
            statusTracker.update(effective,
                effective == ConnectionState::Error ? error : QString{});
        });
        QObject::connect(client, &QOpcUaClient::errorChanged,
                         client, [this](QOpcUaClient::ClientError e) {
            if (e == QOpcUaClient::NoError) return;
            auto const message = QStringLiteral("OPC UA error %1").arg(int(e));
            setLastError(message);
            state.store(ConnectionState::Error, std::memory_order_release);
            statusTracker.update(ConnectionState::Error, message);
        });
        {
            auto* cl = client;
            scheduler->setDelayFn([cl](int ms, std::function<void()> fn) {
                QMetaObject::invokeMethod(cl, [cl, ms, fn = std::move(fn)]() mutable {
                    QTimer::singleShot(ms, cl, [fn = std::move(fn)]() mutable { fn(); });
                });
            });
        }

        client->moveToThread(thread);
        thread->start();
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        autoReconnect.store(false, std::memory_order_release);
        if (scheduler) scheduler->stopAsync();
        if (reconnectTimer) {
            detail::invokeBlocking(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            });
        }
        if (client) {
            detail::invokeBlocking(client, [this] {
                client->disconnectFromEndpoint();
                delete client;
                client = nullptr;
            });
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    void setLastError(QString message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    QString getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    void connectEndpoint() {
        QOpcUaEndpointDescription ep;
        ep.setEndpointUrl(cfg.endpointUrl);
        if (!cfg.username.isEmpty()) {
            QOpcUaAuthenticationInformation auth;
            auth.setUsernameAuthentication(cfg.username, cfg.password);
            client->setAuthenticationInformation(auth);
        }
        client->connectToEndpoint(ep);
    }

    Config                                            cfg;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QOpcUaClient*                                      client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<bool>                                  stopping{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    QString                                            lastError;
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

OpcUaClientTransport::~OpcUaClientTransport() = default;

QString               OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       OpcUaClientTransport::status() const { return m_impl->statusTracker.snapshot(); }

sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
OpcUaClientTransport::connect() {
    if (!m_impl->client) {
        auto const error = m_impl->getLastError();
        return std::unexpected(error.isEmpty()
            ? QStringLiteral("OPC UA backend not initialised")
            : error);
    }
    if (QThread::currentThread() == m_impl->client->thread()) {
        return std::unexpected(QStringLiteral(
            "connect() cannot block the OPC UA worker thread"));
    }
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    m_impl->setLastError({});
    m_impl->state.store(ConnectionState::Connecting,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Connecting);
    detail::invokeBlocking(m_impl->client, [this] {
        m_impl->connectEndpoint();
    });

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) {
            armReconnectIfConfigured();
            return {};
        }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::unexpected(m_impl->getLastError());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto const error = m_impl->getLastError();
    detail::invokeBlocking(m_impl->client, [this] {
        m_impl->client->disconnectFromEndpoint();
    });
    armReconnectIfConfigured();
    auto const message = error.isEmpty()
        ? QStringLiteral("OPC UA connect timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}

void OpcUaClientTransport::disconnect() {
    if (!m_impl->client) return;
    m_impl->autoReconnect.store(false, std::memory_order_release);
    m_impl->setLastError({});
    detail::invokeBlocking(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectFromEndpoint();
    });
    m_impl->state.store(ConnectionState::Disconnected, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

void OpcUaClientTransport::armReconnectIfConfigured() {
    if (!m_impl->client || m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.exchange(true, std::memory_order_acq_rel)) return;
    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;
    detail::invokeBlocking(impl->client, [impl, intervalMs] {
        if (impl->reconnectTimer) {
            if (!impl->reconnectTimer->isActive()) impl->reconnectTimer->start();
            return;
        }
        impl->reconnectTimer = new QTimer(impl->client);
        impl->reconnectTimer->setInterval(intervalMs);
        QObject::connect(impl->reconnectTimer, &QTimer::timeout, impl->client,
            [impl] {
                if (!impl->autoReconnect.load(std::memory_order_acquire)
                    || impl->stopping.load(std::memory_order_acquire)) return;
                auto const s = impl->state.load(std::memory_order_acquire);
                if (s == ConnectionState::Connected
                    || s == ConnectionState::Connecting) return;
                auto const actual = impl->client->state();
                if (actual == QOpcUaClient::Disconnected) {
                    impl->connectEndpoint();
                } else if (actual != QOpcUaClient::Closing
                           && actual != QOpcUaClient::Connecting) {
                    // Finish disconnecting first; the next timer tick starts
                    // a clean connection instead of racing Closing state.
                    impl->client->disconnectFromEndpoint();
                }
            });
        impl->reconnectTimer->start();
    });
}

// Read each address as a separate OPC UA node read. For high-throughput
// applications the caller should use OPC UA monitored items / publish-
// subscribe instead, exposed through a Phase 4 protocol-specific module.
ReadResult OpcUaClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (m_impl->client
        && QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage = QStringLiteral(
            "synchronous read cannot run on the OPC UA worker thread");
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    QList<quint16> out;
    out.reserve(req.count);
    for (int i = 0; i < req.count; ++i) {
        QString const nodeId = m_impl->cfg.nodeIdTemplate.arg(req.startAddress + i);
        auto pending = std::make_shared<PendingNodeRead>();
        detail::invokeBlocking(m_impl->client, [this, nodeId, pending] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) { pending->done.release(); return; }
            QObject::connect(node, &QOpcUaNode::attributeRead, node,
                [pending, node](QOpcUa::NodeAttributes attrs) {
                    if (attrs.testFlag(QOpcUa::NodeAttribute::Value)) {
                        pending->value = node->attribute(QOpcUa::NodeAttribute::Value);
                        pending->ok = true;
                    }
                    node->deleteLater();
                    pending->done.release();
                });
            if (!node->readAttributes(QOpcUa::NodeAttribute::Value)) {
                node->deleteLater();
                pending->done.release();
            }
        });
        if (!pending->done.tryAcquire(1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA read timeout @ %1").arg(nodeId);
            return result;
        }
        if (!pending->ok) {
            result.errorMessage = QStringLiteral("OPC UA read failed @ %1").arg(nodeId);
            return result;
        }
        out.append(quint16(pending->value.toUInt()));
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult OpcUaClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (m_impl->client
        && QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage = QStringLiteral(
            "synchronous write cannot run on the OPC UA worker thread");
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    for (int i = 0; i < batch.values.size(); ++i) {
        QString const nodeId = m_impl->cfg.nodeIdTemplate.arg(batch.startAddress + i);
        QVariant const v = batch.values.at(i);
        auto pending = std::make_shared<PendingNodeWrite>();
        detail::invokeBlocking(m_impl->client, [this, nodeId, v, pending] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) { pending->done.release(); return; }
            QObject::connect(node, &QOpcUaNode::attributeWritten, node,
                [pending, node](QOpcUa::NodeAttribute /*attr*/,
                                QOpcUa::UaStatusCode status) {
                    pending->ok = (status == QOpcUa::UaStatusCode::Good);
                    node->deleteLater();
                    pending->done.release();
                });
            if (!node->writeAttribute(QOpcUa::NodeAttribute::Value, v)) {
                node->deleteLater();
                pending->done.release();
            }
        });
        if (!pending->done.tryAcquire(1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA write timeout @ %1").arg(nodeId);
            return result;
        }
        if (!pending->ok) {
            result.errorMessage = QStringLiteral("OPC UA write rejected @ %1").arg(nodeId);
            return result;
        }
    }
    result.ok = true;
    return result;
}

// --- Async I/O ------------------------------------------------------------
// OPC UA maps one ReadRequest to N node reads. The sync path walks them with a
// BlockingQueued hop + semaphore per node; the async path chains them on the
// client thread: read node[i] -> on attributeRead append + i++ -> read node[i+1]
// -> deliver when i == count. A single overall safety timeout bounds a stuck
// node so it cannot wedge the poll module forever; `completed` makes the timeout
// and the signal path mutually exclusive (whoever fires first wins, once).
void OpcUaClientTransport::readAsync(ReadRequest const& req, ReadDone done) {
    if (state() != ConnectionState::Connected) {
        ReadResult r;
        r.startAddress = req.startAddress;
        r.errorMessage = QStringLiteral("not connected");
        done(std::move(r));
        return;
    }
    struct Chain {
        ReadRequest    req;
        ReadDone       done;
        QList<quint16> out;
        int            i = 0;
        bool           completed = false;
        // `step` is owned by the Chain but captures only client/tpl (never the
        // Chain), so there is no self-referential shared_ptr cycle; liveness
        // across each async gap is carried by the per-node signal lambda, which
        // holds `self` strong and dies with its node (deleteLater).
        std::function<void(std::shared_ptr<Chain>)> step;
    };
    auto  st  = std::make_shared<Chain>();
    st->req   = req;
    st->done  = std::move(done);
    auto* client  = m_impl->client;
    QString tpl   = m_impl->cfg.nodeIdTemplate;
    int safetyMs  = m_impl->cfg.requestTimeoutMs * std::max(1, req.count) + 500;

    QMetaObject::invokeMethod(client, [st, client, tpl, safetyMs]() {
        QTimer::singleShot(safetyMs, client, [st]() {
            if (st->completed) return;
            st->completed = true;
            ReadResult r;
            r.startAddress = st->req.startAddress;
            r.errorMessage = QStringLiteral("OPC UA read timeout");
            st->done(std::move(r));
        });
        st->step = [client, tpl](std::shared_ptr<Chain> self) {
            if (self->completed) return;
            if (self->i >= self->req.count) {
                self->completed = true;
                ReadResult r;
                r.ok           = true;
                r.startAddress = self->req.startAddress;
                r.values       = self->out;
                self->done(std::move(r));
                return;
            }
            QString const nodeId = tpl.arg(self->req.startAddress + self->i);
            auto* node = client->node(nodeId);
            if (!node) {
                self->completed = true;
                ReadResult r;
                r.startAddress = self->req.startAddress;
                r.errorMessage = QStringLiteral("OPC UA bad node %1").arg(nodeId);
                self->done(std::move(r));
                return;
            }
            QObject::connect(node, &QOpcUaNode::attributeRead, node,
                [self, node](QOpcUa::NodeAttributes attrs) {
                    if (self->completed) { node->deleteLater(); return; }
                    bool const ok = attrs.testFlag(QOpcUa::NodeAttribute::Value);
                    if (ok) {
                        self->out.append(quint16(
                            node->attribute(QOpcUa::NodeAttribute::Value).toUInt()));
                    }
                    node->deleteLater();
                    if (!ok) {
                        self->completed = true;
                        ReadResult r;
                        r.startAddress = self->req.startAddress;
                        r.errorMessage = QStringLiteral("OPC UA read failed");
                        self->done(std::move(r));
                        return;
                    }
                    ++self->i;
                    self->step(self);
                });
            if (!node->readAttributes(QOpcUa::NodeAttribute::Value)) {
                node->deleteLater();
                self->completed = true;
                ReadResult r;
                r.startAddress = self->req.startAddress;
                r.errorMessage = QStringLiteral("OPC UA readAttributes failed");
                self->done(std::move(r));
            }
        };
        st->step(st);
    }, Qt::QueuedConnection);
}

void OpcUaClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    struct Chain {
        WriteBatch batch;
        WriteDone  done;
        int        i = 0;
        bool       completed = false;
        std::function<void(std::shared_ptr<Chain>)> step;  // see readAsync note
    };
    auto  st  = std::make_shared<Chain>();
    st->batch = batch;
    st->done  = std::move(done);
    auto* client  = m_impl->client;
    QString tpl   = m_impl->cfg.nodeIdTemplate;
    int const n   = batch.values.size();
    int safetyMs  = m_impl->cfg.requestTimeoutMs * std::max(1, n) + 500;

    QMetaObject::invokeMethod(client, [st, client, tpl, safetyMs]() {
        QTimer::singleShot(safetyMs, client, [st]() {
            if (st->completed) return;
            st->completed = true;
            st->done(WriteResult{false, QStringLiteral("OPC UA write timeout")});
        });
        st->step = [client, tpl](std::shared_ptr<Chain> self) {
            if (self->completed) return;
            if (self->i >= self->batch.values.size()) {
                self->completed = true;
                self->done(WriteResult{true, {}});
                return;
            }
            QString const nodeId =
                tpl.arg(self->batch.startAddress + self->i);
            QVariant const v = self->batch.values.at(self->i);
            auto* node = client->node(nodeId);
            if (!node) {
                self->completed = true;
                self->done(WriteResult{false,
                    QStringLiteral("OPC UA bad node %1").arg(nodeId)});
                return;
            }
            QObject::connect(node, &QOpcUaNode::attributeWritten, node,
                [self, node](QOpcUa::NodeAttribute /*attr*/,
                             QOpcUa::UaStatusCode status) {
                    if (self->completed) { node->deleteLater(); return; }
                    bool const ok = (status == QOpcUa::UaStatusCode::Good);
                    node->deleteLater();
                    if (!ok) {
                        self->completed = true;
                        self->done(WriteResult{false,
                            QStringLiteral("OPC UA write rejected")});
                        return;
                    }
                    ++self->i;
                    self->step(self);
                });
            if (!node->writeAttribute(QOpcUa::NodeAttribute::Value, v)) {
                node->deleteLater();
                self->completed = true;
                self->done(WriteResult{false,
                    QStringLiteral("OPC UA writeAttribute failed")});
            }
        };
        st->step(st);
    }, Qt::QueuedConnection);
}

#else  // !CORE_HAS_OPCUA

class OpcUaClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , statusTracker(cfg.id, TransportKind::OpcUaClient, b, {},
                        EndpointInfo{QUrl(cfg.endpointUrl).host(),
                                     quint16(QUrl(cfg.endpointUrl).port(4840))})
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    detail::TransportStatusTracker          statusTracker;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
OpcUaClientTransport::~OpcUaClientTransport() = default;

QString               OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       OpcUaClientTransport::status() const { return m_impl->statusTracker.snapshot(); }
sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString> OpcUaClientTransport::connect() {
    auto const message = QStringLiteral(
        "OpcUaClientTransport disabled (CORE_BUILD_OPCUA=OFF)");
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}
void OpcUaClientTransport::disconnect() {
    m_impl->state.store(ConnectionState::Disconnected, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}
ReadResult  OpcUaClientTransport::read      (ReadRequest const&)       {
    ReadResult r; r.errorMessage = QStringLiteral("OPC UA disabled at build time");
    return r;
}
WriteResult OpcUaClientTransport::writeBatch(WriteBatch  const&)       {
    WriteResult r; r.errorMessage = QStringLiteral("OPC UA disabled at build time");
    return r;
}
void OpcUaClientTransport::readAsync(ReadRequest const& req, ReadDone done) {
    done(read(req));   // stub returns instantly; no thread is parked
}
void OpcUaClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    done(writeBatch(batch));
}

#endif

} // namespace core::transport
