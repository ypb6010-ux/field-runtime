// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/OpcUaClientTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
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

QString qs(std::string const& s) {
    return QString::fromStdString(s);
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
        , busPtr(b)
        , statusTracker(
              cfg.id, TransportKind::OpcUaClient, b, {},
              EndpointInfo{
                  QUrl(qs(cfg.endpointUrl)).host().toStdString(),
                  static_cast<std::uint16_t>(
                      QUrl(qs(cfg.endpointUrl)).port(4840))})
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread) {

        // Create the OPC UA client on the caller thread (provider plugins
        // load synchronously), then ship it over to the worker so all
        // subsequent network I/O runs there. invokeMethod-on-QThread from
        // the same thread deadlocks, so we never use that pattern.
        QOpcUaProvider provider;
        client = provider.createClient(qs(cfg.backend));
        if (!client) {
            auto const message =
                QStringLiteral("provider failed to create '%1' backend")
                    .arg(qs(cfg.backend)).toStdString();
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
            auto const effective =
                cur == ConnectionState::Disconnected && !error.empty()
                ? ConnectionState::Error : cur;
            auto const prev =
                state.exchange(effective, std::memory_order_acq_rel);
            statusTracker.update(
                effective,
                effective == ConnectionState::Error ? error : std::string{});
            if (!busPtr) return;
            if (effective == ConnectionState::Connected
                && prev != ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Connected, {}});
            } else if (effective != ConnectionState::Connected
                       && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, error});
            }
        });
        QObject::connect(client, &QOpcUaClient::errorChanged,
                         client, [this](QOpcUaClient::ClientError e) {
            if (e == QOpcUaClient::NoError) return;
            auto const message =
                QStringLiteral("OPC UA error %1").arg(int(e)).toStdString();
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
        if (scheduler) scheduler->stopAsync();
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

    void setLastError(std::string message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    std::string getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QOpcUaClient*                                      client = nullptr;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    std::string                                        lastError;
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

OpcUaClientTransport::~OpcUaClientTransport() = default;

std::string           OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       OpcUaClientTransport::status() const {
    return m_impl->statusTracker.snapshot();
}

sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, std::string>
OpcUaClientTransport::connect() {
    if (!m_impl->client) {
        auto const error = m_impl->getLastError();
        return std::unexpected(error.empty()
            ? std::string("OPC UA backend not initialised")
            : error);
    }
    if (QThread::currentThread() == m_impl->client->thread()) {
        return std::unexpected(
            std::string("connect() cannot block the OPC UA worker thread"));
    }
    if (state() == ConnectionState::Connected) return {};
    m_impl->setLastError({});
    m_impl->state.store(ConnectionState::Connecting,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Connecting);
    detail::invokeBlocking(m_impl->client, [this] {
        QOpcUaEndpointDescription ep;
        ep.setEndpointUrl(qs(m_impl->cfg.endpointUrl));
        if (!m_impl->cfg.username.empty()) {
            QOpcUaAuthenticationInformation auth;
            auth.setUsernameAuthentication(qs(m_impl->cfg.username), qs(m_impl->cfg.password));
            m_impl->client->setAuthenticationInformation(auth);
        }
        m_impl->client->connectToEndpoint(ep);
    });

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) return {};
        if (s == ConnectionState::Error)
            return std::unexpected(m_impl->getLastError());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto const error = m_impl->getLastError();
    auto const message =
        error.empty() ? std::string("OPC UA connect timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}

void OpcUaClientTransport::disconnect() {
    if (m_impl->client) {
        m_impl->setLastError({});
        detail::invokeBlocking(m_impl->client, [this] {
            m_impl->client->disconnectFromEndpoint();
        });
    }
    m_impl->state.store(ConnectionState::Disconnected,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

// Read each address as a separate OPC UA node read. For high-throughput
// applications the caller should use OPC UA monitored items / publish-
// subscribe instead, exposed through a Phase 4 protocol-specific module.
ReadResult OpcUaClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (m_impl->client
        && QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage =
            "synchronous read cannot run on the OPC UA worker thread";
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
        return result;
    }
    core::RegisterWords out;
    out.reserve(req.count);
    for (int i = 0; i < req.count; ++i) {
        QString const nodeId = qs(m_impl->cfg.nodeIdTemplate).arg(req.startAddress + i);
        auto pending = std::make_shared<PendingNodeRead>();
        detail::invokeBlocking(m_impl->client, [this, nodeId, pending] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) {
                pending->done.release();
                return;
            }
            QObject::connect(node, &QOpcUaNode::attributeRead, node,
                [pending, node](QOpcUa::NodeAttributes attrs) {
                    if (attrs.testFlag(QOpcUa::NodeAttribute::Value)) {
                        pending->value =
                            node->attribute(QOpcUa::NodeAttribute::Value);
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
        if (!pending->done.tryAcquire(
                1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA read timeout @ %1").arg(nodeId).toStdString();
            return result;
        }
        if (!pending->ok) {
            result.errorMessage = QStringLiteral("OPC UA read failed @ %1").arg(nodeId).toStdString();
            return result;
        }
        out.push_back(quint16(pending->value.toUInt()));
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult OpcUaClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (m_impl->client
        && QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage =
            "synchronous write cannot run on the OPC UA worker thread";
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
        return result;
    }
    for (int i = 0; i < batch.values.size(); ++i) {
        QString const nodeId = qs(m_impl->cfg.nodeIdTemplate).arg(batch.startAddress + i);
        QVariant const v = batch.values.at(i);
        auto pending = std::make_shared<PendingNodeWrite>();
        detail::invokeBlocking(
            m_impl->client, [this, nodeId, v, pending] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) {
                pending->done.release();
                return;
            }
            QObject::connect(node, &QOpcUaNode::attributeWritten, node,
                [pending, node](QOpcUa::NodeAttribute /*attr*/,
                                QOpcUa::UaStatusCode status) {
                    pending->ok =
                        (status == QOpcUa::UaStatusCode::Good);
                    node->deleteLater();
                    pending->done.release();
                });
            if (!node->writeAttribute(QOpcUa::NodeAttribute::Value, v)) {
                node->deleteLater();
                pending->done.release();
            }
        });
        if (!pending->done.tryAcquire(
                1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA write timeout @ %1").arg(nodeId).toStdString();
            return result;
        }
        if (!pending->ok) {
            result.errorMessage = QStringLiteral("OPC UA write rejected @ %1").arg(nodeId).toStdString();
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
        r.errorMessage = "not connected";
        done(std::move(r));
        return;
    }
    struct Chain {
        ReadRequest    req;
        ReadDone       done;
        core::RegisterWords out;
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
    QString tpl   = qs(m_impl->cfg.nodeIdTemplate);
    int safetyMs  = m_impl->cfg.requestTimeoutMs * std::max(1, req.count) + 500;

    QMetaObject::invokeMethod(client, [st, client, tpl, safetyMs]() {
        QTimer::singleShot(safetyMs, client, [st]() {
            if (st->completed) return;
            st->completed = true;
            ReadResult r;
            r.startAddress = st->req.startAddress;
            r.errorMessage = "OPC UA read timeout";
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
                r.errorMessage = QStringLiteral("OPC UA bad node %1").arg(nodeId).toStdString();
                self->done(std::move(r));
                return;
            }
            QObject::connect(node, &QOpcUaNode::attributeRead, node,
                [self, node](QOpcUa::NodeAttributes attrs) {
                    if (self->completed) { node->deleteLater(); return; }
                    bool const ok = attrs.testFlag(QOpcUa::NodeAttribute::Value);
                    if (ok) {
                        self->out.push_back(quint16(
                            node->attribute(QOpcUa::NodeAttribute::Value).toUInt()));
                    }
                    node->deleteLater();
                    if (!ok) {
                        self->completed = true;
                        ReadResult r;
                        r.startAddress = self->req.startAddress;
                        r.errorMessage = "OPC UA read failed";
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
                r.errorMessage = "OPC UA readAttributes failed";
                self->done(std::move(r));
            }
        };
        st->step(st);
    }, Qt::QueuedConnection);
}

void OpcUaClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, "not connected"});
        return;
    }
    if (batch.values.size()
        > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        done(WriteResult{false, "write batch is too large"});
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
    QString tpl   = qs(m_impl->cfg.nodeIdTemplate);
    int const n = static_cast<int>(batch.values.size());
    auto const safetyMs64 =
        std::int64_t(m_impl->cfg.requestTimeoutMs) * std::max(1, n) + 500;
    int const safetyMs = static_cast<int>(
        std::min<std::int64_t>(safetyMs64,
                               std::numeric_limits<int>::max()));

    QMetaObject::invokeMethod(client, [st, client, tpl, safetyMs]() {
        QTimer::singleShot(safetyMs, client, [st]() {
            if (st->completed) return;
            st->completed = true;
            st->done(WriteResult{false, "OPC UA write timeout"});
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
                    QStringLiteral("OPC UA bad node %1").arg(nodeId).toStdString()});
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
                            "OPC UA write rejected"});
                        return;
                    }
                    ++self->i;
                    self->step(self);
                });
            if (!node->writeAttribute(QOpcUa::NodeAttribute::Value, v)) {
                node->deleteLater();
                self->completed = true;
                self->done(WriteResult{false,
                    "OPC UA writeAttribute failed"});
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
        , statusTracker(
              cfg.id, TransportKind::OpcUaClient, b, {},
              EndpointInfo{
                  QUrl(QString::fromStdString(cfg.endpointUrl))
                      .host().toStdString(),
                  static_cast<std::uint16_t>(
                      QUrl(QString::fromStdString(cfg.endpointUrl))
                          .port(4840))})
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    detail::TransportStatusTracker          statusTracker;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
OpcUaClientTransport::~OpcUaClientTransport() = default;

std::string           OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       OpcUaClientTransport::status() const {
    return m_impl->statusTracker.snapshot();
}
sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, std::string> OpcUaClientTransport::connect() {
    auto const message =
        std::string("OpcUaClientTransport disabled (CORE_BUILD_OPCUA=OFF)");
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}
void OpcUaClientTransport::disconnect() {
    m_impl->state.store(ConnectionState::Disconnected,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}
ReadResult  OpcUaClientTransport::read      (ReadRequest const&)       {
    ReadResult r; r.errorMessage = "OPC UA disabled at build time";
    return r;
}
WriteResult OpcUaClientTransport::writeBatch(WriteBatch  const&)       {
    WriteResult r; r.errorMessage = "OPC UA disabled at build time";
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
