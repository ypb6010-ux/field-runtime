// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/OpcUaClientTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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

} // namespace

class OpcUaClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread) {

        // Create the OPC UA client on the caller thread (provider plugins
        // load synchronously), then ship it over to the worker so all
        // subsequent network I/O runs there. invokeMethod-on-QThread from
        // the same thread deadlocks, so we never use that pattern.
        QOpcUaProvider provider;
        client = provider.createClient(cfg.backend);
        if (!client) {
            lastError = QStringLiteral("provider failed to create '%1' backend")
                            .arg(cfg.backend);
            thread->start();   // keep the worker thread alive for symmetry
            return;
        }
        client->setApplicationIdentity({});
        QObject::connect(client, &QOpcUaClient::stateChanged,
                         client, [this](QOpcUaClient::ClientState s) {
            auto const cur  = stateFromQt(s);
            auto const prev = state.exchange(cur, std::memory_order_acq_rel);
            if (!busPtr) return;
            if (cur == ConnectionState::Connected
                && prev != ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id.toStdString(), bus::TransportEventKind::Connected, {}});
            } else if (cur == ConnectionState::Disconnected
                       && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id.toStdString(), bus::TransportEventKind::Disconnected, {}});
            }
        });
        QObject::connect(client, &QOpcUaClient::errorChanged,
                         client, [this](QOpcUaClient::ClientError e) {
            if (e == QOpcUaClient::NoError) return;
            lastError = QStringLiteral("OPC UA error %1").arg(int(e));
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
            QMetaObject::invokeMethod(client, [this] {
                client->disconnectFromEndpoint();
                delete client;
                client = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QOpcUaClient*                                      client = nullptr;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

OpcUaClientTransport::~OpcUaClientTransport() = default;

QString               OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
OpcUaClientTransport::connect() {
    if (!m_impl->client) {
        return std::unexpected(m_impl->lastError.isEmpty()
            ? QStringLiteral("OPC UA backend not initialised")
            : m_impl->lastError);
    }
    if (state() == ConnectionState::Connected) return {};
    QMetaObject::invokeMethod(m_impl->client, [this] {
        QOpcUaEndpointDescription ep;
        ep.setEndpointUrl(m_impl->cfg.endpointUrl);
        if (!m_impl->cfg.username.isEmpty()) {
            QOpcUaAuthenticationInformation auth;
            auth.setUsernameAuthentication(m_impl->cfg.username, m_impl->cfg.password);
            m_impl->client->setAuthenticationInformation(auth);
        }
        m_impl->client->connectToEndpoint(ep);
    }, Qt::BlockingQueuedConnection);

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) return {};
        if (s == ConnectionState::Error)
            return std::unexpected(m_impl->lastError);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::unexpected(QStringLiteral("OPC UA connect timeout"));
}

void OpcUaClientTransport::disconnect() {
    if (!m_impl->client) return;
    QMetaObject::invokeMethod(m_impl->client, [this] {
        m_impl->client->disconnectFromEndpoint();
    }, Qt::BlockingQueuedConnection);
}

// Read each address as a separate OPC UA node read. For high-throughput
// applications the caller should use OPC UA monitored items / publish-
// subscribe instead, exposed through a Phase 4 protocol-specific module.
ReadResult OpcUaClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    core::RegisterWords out;
    out.reserve(req.count);
    for (int i = 0; i < req.count; ++i) {
        QString const nodeId = m_impl->cfg.nodeIdTemplate.arg(req.startAddress + i);
        QSemaphore done(0);
        QVariant   value;
        bool       ok = false;
        QMetaObject::invokeMethod(m_impl->client, [&] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) { done.release(); return; }
            QObject::connect(node, &QOpcUaNode::attributeRead, node,
                [&done, &value, &ok, node](QOpcUa::NodeAttributes attrs) {
                    if (attrs.testFlag(QOpcUa::NodeAttribute::Value)) {
                        value = node->attribute(QOpcUa::NodeAttribute::Value);
                        ok = true;
                    }
                    node->deleteLater();
                    done.release();
                });
            if (!node->readAttributes(QOpcUa::NodeAttribute::Value)) {
                node->deleteLater();
                done.release();
            }
        }, Qt::BlockingQueuedConnection);
        if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA read timeout @ %1").arg(nodeId);
            return result;
        }
        if (!ok) {
            result.errorMessage = QStringLiteral("OPC UA read failed @ %1").arg(nodeId);
            return result;
        }
        out.push_back(quint16(value.toUInt()));
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult OpcUaClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    for (int i = 0; i < batch.values.size(); ++i) {
        QString const nodeId = m_impl->cfg.nodeIdTemplate.arg(batch.startAddress + i);
        QSemaphore done(0);
        bool       ok = false;
        QVariant const v = batch.values.at(i);
        QMetaObject::invokeMethod(m_impl->client, [&] {
            auto* node = m_impl->client->node(nodeId);
            if (!node) { done.release(); return; }
            QObject::connect(node, &QOpcUaNode::attributeWritten, node,
                [&done, &ok, node](QOpcUa::NodeAttribute /*attr*/,
                                    QOpcUa::UaStatusCode status) {
                    ok = (status == QOpcUa::UaStatusCode::Good);
                    node->deleteLater();
                    done.release();
                });
            if (!node->writeAttribute(QOpcUa::NodeAttribute::Value, v)) {
                node->deleteLater();
                done.release();
            }
        }, Qt::BlockingQueuedConnection);
        if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs)) {
            result.errorMessage = QStringLiteral("OPC UA write timeout @ %1").arg(nodeId);
            return result;
        }
        if (!ok) {
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
                        self->out.push_back(quint16(
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
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    bus::EventBus*                          busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
OpcUaClientTransport::~OpcUaClientTransport() = default;

QString               OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }
sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString> OpcUaClientTransport::connect() {
    return std::unexpected(QStringLiteral(
        "OpcUaClientTransport disabled (CORE_BUILD_OPCUA=OFF)"));
}
void        OpcUaClientTransport::disconnect()                        {}
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
