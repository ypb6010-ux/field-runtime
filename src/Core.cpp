#include "core/ICore.h"
#include "core/internal/Testing.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QHash>
#include <QQmlContext>
#include <QTimer>
#include <QtSerialBus/QModbusDataUnit>

#include "core/bus/EventBus.h"
#include "core/bus/BusEvents.h"
#include "core/bus/Subscription.h"
#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/log/Logger.h"
#include "core/log/Sinks.h"
#include "core/qml/LogBridge.h"
#include "core/module/AckWatch.h"
#include "core/module/Command.h"
#include "core/module/Heartbeat.h"
#include "core/module/ModuleRegistry.h"
#include "core/module/PollRange.h"
#include "core/module/SinkWindow.h"
#include "core/plugin/PluginRegistry.h"
#include "core/transport/ModbusRtuTransport.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/ModbusTcpServerTransport.h"
#include "core/transport/MqttClientTransport.h"
#include "core/transport/MqttPahoTransport.h"
#include "core/transport/OpcUaClientTransport.h"
#include "core/transport/S7ClientTransport.h"
#include "core/transport/Transport.h"

namespace core {

namespace {

QModbusDataUnit::RegisterType tableFromString(QString const& s) {
    static const QHash<QString, QModbusDataUnit::RegisterType> map = {
        {"HR",                QModbusDataUnit::HoldingRegisters},
        {"HoldingRegisters",  QModbusDataUnit::HoldingRegisters},
        {"IR",                QModbusDataUnit::InputRegisters},
        {"InputRegisters",    QModbusDataUnit::InputRegisters},
        {"Coil",              QModbusDataUnit::Coils},
        {"Coils",             QModbusDataUnit::Coils},
        {"DI",                QModbusDataUnit::DiscreteInputs},
        {"DiscreteInputs",    QModbusDataUnit::DiscreteInputs},
    };
    return map.value(s, QModbusDataUnit::HoldingRegisters);
}

dp::WordOrder wordOrderFromString(QString const& s) {
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    return dp::WordOrder::ABCD;
}

dp::Kind kindFromString(QString const& s) {
    if (s == "Command")       return dp::Kind::Command;
    if (s == "Bidirectional") return dp::Kind::Bidirectional;
    return dp::Kind::Status;
}

dp::PortRef makePortRef(config::PortRefConfig const& pc,
                         std::shared_ptr<codec::Codec> codec) {
    dp::PortRef p;
    p.transport = pc.port;
    p.table     = tableFromString(pc.table);
    p.address   = pc.address;
    if (pc.bit >= 0) p.bit = pc.bit;
    p.wordOrder = wordOrderFromString(pc.wordOrder);
    p.shift     = pc.shift;
    p.mask      = pc.mask;
    p.scale     = pc.scale;
    p.offset    = pc.offset;
    p.codec     = std::move(codec);
    p.window    = pc.window;
    return p;
}

} // namespace

class CoreImpl : public ICore {
public:
    explicit CoreImpl(QQmlContext* qml)
        : m_qml(qml)
        , m_logger(std::make_unique<log::Logger>())
        , m_bus(std::make_unique<bus::EventBus>())
        , m_codecs(std::make_unique<codec::CodecRegistry>())
        , m_dps(std::make_unique<dp::DatapointRegistry>())
        , m_modules(std::make_unique<module::ModuleRegistry>())
        , m_plugins(std::make_unique<plugin::PluginRegistry>()) {
        m_codecs->loadBuiltins();
        // Built-in console sink so diagnostics surface out of the box; the app
        // adds file / DB sinks via logger().addSink(...).
        m_logger->addSink(std::make_shared<log::ConsoleSink>());
        installLogBridge();
    }

    ~CoreImpl() override {
        // Async-safe teardown order:
        //   1. drop event-bus subscriptions (no more routing into modules);
        //   2. stop the module tick driver so no NEW driveTick() is issued
        //      (modules stay alive);
        //   3. destroy the transports — each stops its scheduler's async pump
        //      (stopAsync) then joins its worker thread, SAFELY ABANDONING any
        //      in-flight completion (a pending reply may be cancelled, not
        //      delivered). Whatever completion does still run invokes module
        //      callbacks (apply result / clear in-flight), so the modules MUST
        //      still be alive here — hence transports before modules. The
        //      modules' raw Transport* go dangling but are never dereferenced
        //      after the tick driver stopped;
        //   4. now destroy the modules;
        //   5. datapoints / codecs / bus, then the logger last.
        m_transportEventSub.reset();
        m_serverWriteSub.reset();
        if (m_modules) m_modules->stopAll();   // stop ticks; keep modules alive
        m_transports.clear();                  // join worker threads → drain in-flight
        m_pollRangePtrs.clear();
        m_sinkWindowPtrs.clear();
        m_modules.reset();                     // safe: no completion can fire now
        m_plugins.reset();
        m_dps.reset();
        m_codecs.reset();
        m_bus.reset();
        // Logger last — subsystems above may log during teardown.
        m_logBridge.reset();
        if (m_logger) m_logger->stop();
        m_logger.reset();
    }

    std::expected<void, config::ValidationErrors>
    loadConfig(QString const& path) override {
        config::ConfigLoader loader;
        auto schema = loader.loadFromToml(path);
        if (!schema.has_value()) {
            for (auto const& err : schema.error()) {
                m_logger->logf(log::LogLevel::Error,
                               QStringLiteral("config"), path,
                               err.message);
            }
            return std::unexpected(schema.error());
        }

        if (!schema->meta.logLevel.isEmpty()) {
            m_logger->setThreshold(log::levelFromString(schema->meta.logLevel));
        }
        wireFromSchema(*schema);
        m_logger->logf(log::LogLevel::Info, QStringLiteral("config"), path,
                       QStringLiteral("loaded"),
                       {{QStringLiteral("transports"),
                         int(schema->transports.size())},
                        {QStringLiteral("datapoints"),
                         int(schema->datapoints.size())}});
        return {};
    }

    bus::EventBus&            bus()        override { return *m_bus; }
    dp::DatapointRegistry&    datapoints() override { return *m_dps; }
    codec::CodecRegistry&     codecs()     override { return *m_codecs; }
    module::ModuleRegistry&   modules()    override { return *m_modules; }
    plugin::PluginRegistry&   plugins()    override { return *m_plugins; }
    log::Logger&              logger()     override { return *m_logger; }

    transport::Transport* transport(QString const& id) const override {
        auto it = m_transports.find(id);
        return it == m_transports.end() ? nullptr : it->second.get();
    }

    QStringList transportIds() const override {
        QStringList ids;
        ids.reserve(int(m_transports.size()));
        for (auto const& [id, t] : m_transports) ids << id;
        return ids;
    }

    void start() override {
        installEventWiring();
        for (auto& [id, t] : m_transports) {
            (void)t->connect();
        }
        m_modules->startAll();
        startStatsPump();
        m_bus->publish(bus::CoreReady{});
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("started"));
    }

    void stop() override {
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("stopping"));
        m_bus->publish(bus::CoreStopping{});
        stopStatsPump();
        m_modules->stopAll();
        for (auto& [id, t] : m_transports) t->disconnect();
        m_logger->flush();
    }

    void pollAllOnce() {
        for (auto* poll : m_pollRangePtrs) poll->pollOnce();
    }

    void tickSinkWindowsOnce() {
        for (auto* sw : m_sinkWindowPtrs) sw->onTick();
    }

    void publishSchedulerStatsOnce() {
        for (auto& [id, t] : m_transports) {
            m_bus->publish(bus::SchedulerStatsEvent{id, t->scheduler().stats()});
        }
    }

    void setSchedulerStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }

private:
    void wireFromSchema(config::ConfigSchema const& schema) {
        registerCustomCodecs(schema);
        buildTransports(schema);
        auto byId = buildDatapoints(schema);
        buildPollRanges(schema, byId);
        buildSinkWindows(schema);
        buildHeartbeats(schema);
        buildAckWatches(schema);
        buildCommands(schema);
        m_routes = schema.routes;
    }

    void startStatsPump() {
        if (m_statsIntervalMs <= 0) return;
        if (m_statsTimer) return;
        m_statsTimer = new QTimer(m_bus.get());
        m_statsTimer->setInterval(m_statsIntervalMs);
        m_statsTimer->setSingleShot(false);
        QObject::connect(m_statsTimer, &QTimer::timeout, m_bus.get(),
            [this]() { publishSchedulerStatsOnce(); });
        m_statsTimer->start();
    }

    void stopStatsPump() {
        if (!m_statsTimer) return;
        m_statsTimer->stop();
        m_statsTimer->deleteLater();
        m_statsTimer = nullptr;
    }

    void installLogBridge() {
        if (!m_qml) return;
        m_logBridge = std::make_unique<qml::LogBridge>(*m_logger);
        m_qml->setContextProperty(QStringLiteral("log"), m_logBridge.get());
    }

    void installEventWiring() {
        // On Transport reconnect (Connected after Disconnected), force every
        // SinkWindow attached to that transport to flush its whole snapshot
        // so PLCs come back with a fresh full picture rather than waiting on
        // sporadic stages.
        m_transportEventSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::TransportEvent>(
                [this](bus::TransportEvent const& e) {
                    logTransportEvent(e);
                    if (e.kind != bus::TransportEventKind::Connected) return;
                    for (auto* sw : m_sinkWindowPtrs) {
                        if (sw->transportId() == e.transportId) sw->forceFlush();
                    }
                }));
        // On operator-box write into a server transport, fan out into a
        // SinkWindow on the PLC-side transport via the configured routes.
        m_serverWriteSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::ServerWriteEvent>(
                [this](bus::ServerWriteEvent const& e) {
                    m_logger->logOperation(log::OperationRecord{
                        {}, QStringLiteral("operator-box"),
                        QStringLiteral("server-write"), e.transportId,
                        {}, QString::number(e.values.size()) + " regs @ "
                            + QString::number(e.startAddress),
                        QStringLiteral("ok"), {}});
                    routeServerWrite(e);
                }));
    }

    void logTransportEvent(bus::TransportEvent const& e) {
        char const* what = nullptr;
        auto level = log::LogLevel::Info;
        switch (e.kind) {
            case bus::TransportEventKind::Connected:       what = "connected"; break;
            case bus::TransportEventKind::Disconnected:    what = "disconnected";
                                                           level = log::LogLevel::Warn; break;
            case bus::TransportEventKind::CircuitOpened:   what = "circuit opened";
                                                           level = log::LogLevel::Error; break;
            case bus::TransportEventKind::CircuitClosed:   what = "circuit closed"; break;
            case bus::TransportEventKind::CircuitHalfOpen: what = "circuit half-open"; break;
            default: return;   // Read/WriteCompleted are too noisy for the log
        }
        m_logger->logf(level, QStringLiteral("transport"), e.transportId,
                       QString::fromLatin1(what));
    }

    void routeServerWrite(bus::ServerWriteEvent const& e) {
        // Routes operate on datapoint IDs; for the server→PLC path, we look
        // at each `from` datapoint whose source is the server transport and
        // whose source address falls within the written range, then call
        // stageRegister on the SinkWindow associated with the `to` datapoint.
        for (auto const& r : m_routes) {
            auto fromIt = m_datapointById.find(r.from);
            auto toIt   = m_datapointById.find(r.to);
            if (fromIt == m_datapointById.end() || toIt == m_datapointById.end()) continue;
            auto const& fromDp = fromIt->second;
            auto const& toDp   = toIt->second;
            if (!fromDp->source().has_value()) continue;
            auto const& fromSrc = *fromDp->source();
            if (fromSrc.transport != e.transportId) continue;
            if (fromSrc.table     != e.table)       continue;
            int const offset = fromSrc.address - e.startAddress;
            if (offset < 0 || offset >= e.values.size()) continue;
            quint16 const raw = e.values.at(offset);

            // Resolve which SinkWindow owns the sink-side register.
            if (!toDp->sink().has_value()) continue;
            auto const& sink = *toDp->sink();
            module::SinkWindow* target = nullptr;
            // Look up by named window first, fall back to address-based match.
            for (auto* sw : m_sinkWindowPtrs) {
                if (!sink.window.isEmpty() && sw->id() == sink.window) {
                    target = sw;
                    break;
                }
            }
            if (!target && !sink.transport.isEmpty()) {
                for (auto* sw : m_sinkWindowPtrs) {
                    if (sw->transportId() != sink.transport) continue;
                    int const addr = sink.address;
                    if (addr < sw->startAddress()
                     || addr >= sw->startAddress() + sw->size()) continue;
                    target = sw;
                    break;
                }
            }
            if (!target) continue;
            int const addr = sink.address;
            if (addr < target->startAddress()
             || addr >= target->startAddress() + target->size()) continue;
            quint16 mask = quint16(sink.mask & 0xFFFFu);
            if (mask == 0) mask = 0xFFFFu;
            target->stageRegister(addr, raw, mask);
        }
    }

    void registerCustomCodecs(config::ConfigSchema const& schema) {
        for (auto const& cc : schema.codecs) {
            if (cc.kind == "enum_u16") {
                std::unordered_map<quint16, QString> map;
                for (auto it = cc.map.constBegin(); it != cc.map.constEnd(); ++it) {
                    bool ok = false;
                    quint16 raw = quint16(it.key().toUInt(&ok));
                    if (!ok) continue;
                    map.emplace(raw, it.value().toString());
                }
                m_codecs->registerCodec(
                    std::make_shared<codec::EnumU16Codec>(cc.id, std::move(map)));
            }
        }
    }

    void buildTransports(config::ConfigSchema const& schema) {
        for (auto const& tc : schema.transports) {
            if (tc.kind == transport::TransportKind::ModbusTcpClient) {
                transport::ModbusTcpClientTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.host                 = tc.host;
                cfg.port                 = quint16(tc.port);
                cfg.slaveId              = tc.slaveId;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::ModbusTcpClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::ModbusTcpServer) {
                transport::ModbusTcpServerTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.listenAddress        = tc.listenAddress;
                cfg.listenPort           = quint16(tc.listenPort);
                cfg.slaveId              = tc.slaveId;
                cfg.maxClients           = tc.maxClients;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.listenRanges         = tc.listenRanges;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::ModbusTcpServerTransport>(
                        std::move(cfg), *m_bus));
            } else if (tc.kind == transport::TransportKind::ModbusRtu) {
                transport::ModbusRtuTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.portName             = tc.portName;
                cfg.baudRate             = tc.baudRate;
                cfg.dataBits             = tc.dataBits;
                cfg.stopBits             = tc.stopBits;
                if      (tc.parity == "even") cfg.parity = transport::ModbusRtuTransport::Parity::Even;
                else if (tc.parity == "odd")  cfg.parity = transport::ModbusRtuTransport::Parity::Odd;
                else                          cfg.parity = transport::ModbusRtuTransport::Parity::None;
                cfg.slaveId              = tc.slaveId;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::ModbusRtuTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::OpcUaClient) {
                transport::OpcUaClientTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.endpointUrl          = tc.endpointUrl;
                cfg.securityPolicy       = tc.securityPolicy;
                cfg.username             = tc.username;
                cfg.password             = tc.password;
                cfg.backend              = tc.opcuaBackend;
                cfg.nodeIdTemplate       = tc.nodeIdTemplate;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::OpcUaClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::MqttClient) {
                transport::MqttClientTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.brokerUri            = tc.brokerUri;
                cfg.clientId             = tc.clientId;
                cfg.username             = tc.username;
                cfg.password             = tc.password;
                cfg.topicPrefix          = tc.topicPrefix;
                cfg.topicTemplate        = tc.topicTemplate;
                cfg.qos                  = tc.qos;
                cfg.cleanSession         = tc.cleanSession;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::MqttClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::MqttPahoClient) {
                transport::MqttPahoTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.brokerUri            = tc.brokerUri;
                cfg.clientId             = tc.clientId;
                cfg.username             = tc.username;
                cfg.password             = tc.password;
                cfg.topicPrefix          = tc.topicPrefix;
                cfg.topicTemplate        = tc.topicTemplate;
                cfg.qos                  = tc.qos;
                cfg.cleanSession         = tc.cleanSession;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::MqttPahoTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::S7Client) {
                transport::S7ClientTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.host                 = tc.host;
                cfg.port                 = tc.port;
                cfg.rack                 = tc.rack;
                cfg.slot                 = tc.slot;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    tc.id,
                    std::make_unique<transport::S7ClientTransport>(
                        std::move(cfg), m_bus.get()));
            }
        }
    }

    using DpById = std::map<QString, std::shared_ptr<dp::Datapoint>>;

    void buildSinkWindows(config::ConfigSchema const& schema) {
        for (auto const& sc : schema.sinkWindows) {
            auto* t = transport(sc.transport);
            if (!t) continue;
            module::SinkWindow::Config cfg;
            cfg.moduleId          = sc.moduleId;
            cfg.table             = tableFromString(sc.table);
            cfg.startAddress      = sc.startAddress;
            cfg.size              = sc.size;
            cfg.priority          = sc.priority;
            cfg.debounceMs        = sc.flush.debounceMs;
            cfg.keepAlivePeriodMs = sc.flush.keepaliveMs;
            cfg.coalesceWrites    = sc.flush.coalesceWrites;
            cfg.initial           = sc.initial;
            auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *t);
            auto* raw = sw.get();
            m_modules->registerModule(std::move(sw));
            m_sinkWindowPtrs.push_back(raw);
        }
    }

    void buildHeartbeats(config::ConfigSchema const& schema) {
        for (auto const& hc : schema.heartbeats) {
            auto* t = transport(hc.transport);
            if (!t) continue;
            module::Heartbeat::Config cfg;
            cfg.moduleId = hc.moduleId;
            cfg.table    = tableFromString(hc.table);
            cfg.address  = hc.address;
            cfg.values   = hc.values;
            cfg.periodMs = hc.periodMs;
            cfg.priority = hc.priority;
            m_modules->registerModule(
                std::make_unique<module::Heartbeat>(std::move(cfg), *t));
        }
    }

    void buildAckWatches(config::ConfigSchema const& schema) {
        for (auto const& ac : schema.ackWatches) {
            module::AckWatch::Config cfg;
            cfg.moduleId  = ac.moduleId;
            cfg.dpId      = ac.dp;
            cfg.expected  = ac.expected;
            cfg.timeoutMs = ac.timeoutMs;
            m_modules->registerModule(
                std::make_unique<module::AckWatch>(std::move(cfg), *m_bus));
        }
    }

    void buildCommands(config::ConfigSchema const& schema) {
        for (auto const& cc : schema.commands) {
            auto* t = transport(cc.transport);
            if (!t) continue;
            module::Command::Config cfg;
            cfg.moduleId      = cc.moduleId;
            cfg.priority      = cc.priority;
            cfg.interruptable = cc.interruptable;
            for (auto const& w : cc.writes) {
                module::Command::Entry e;
                e.table   = tableFromString(w.table);
                e.address = w.address;
                e.value   = w.value;
                cfg.writes.append(e);
            }
            m_modules->registerModule(
                std::make_unique<module::Command>(std::move(cfg), *t));
        }
    }

    DpById buildDatapoints(config::ConfigSchema const& schema) {
        DpById out;
        for (auto const& dc : schema.datapoints) {
            std::shared_ptr<codec::Codec> sourceCodec;
            if (dc.hasSource) {
                if (!dc.source.codec.isEmpty()) {
                    sourceCodec = m_codecs->find(dc.source.codec);
                }
                if (!sourceCodec) {
                    sourceCodec = m_codecs->find(
                        codec::BuiltinScalarCodec::idFor(dc.type));
                }
            }

            dp::DatapointSpec spec;
            spec.id         = dc.id;
            spec.kind       = kindFromString(dc.kind);
            spec.type       = dc.type;
            if (dc.hasSource) spec.source = makePortRef(dc.source, sourceCodec);
            if (dc.hasSink) {
                spec.sink = makePortRef(dc.sink,
                    m_codecs->find(codec::BuiltinScalarCodec::idFor(dc.type)));
            }
            spec.uiBinding  = dc.ui;
            spec.persistTag = dc.persist;

            auto datapoint = std::make_shared<dp::Datapoint>(std::move(spec));
            m_dps->registerDp(datapoint);
            out.emplace(dc.id, datapoint);
            m_datapointById.emplace(dc.id, datapoint);

            // Auto-publish DpChanged on every value change so plugins /
            // database / dashboard subscribers don't need to poll.
            std::weak_ptr<dp::Datapoint> weak = datapoint;
            QObject::connect(datapoint.get(), &dp::Datapoint::valueChanged,
                m_bus.get(), [this, weak] {
                    auto sp = weak.lock();
                    if (!sp) return;
                    m_bus->publish(bus::DpChanged{
                        sp->id(), sp->value(), sp->timestamp()});
                });

            // For Command / Bidirectional datapoints with a SinkWindow sink,
            // wire a writer that encodes the value through the codec then
            // stages into the named SinkWindow. QML calls `dp.write(v)` and
            // the new value lands on the PLC at the next flush tick.
            if ((datapoint->kind() == dp::Kind::Command
                 || datapoint->kind() == dp::Kind::Bidirectional)
                && datapoint->sink().has_value()
                && !datapoint->sink()->window.isEmpty()) {
                QString const windowId = datapoint->sink()->window;
                datapoint->setWriter([this, weak, windowId](QVariant v) {
                    auto sp = weak.lock();
                    if (!sp || !sp->sink().has_value()) return;
                    auto sink = *sp->sink();
                    if (!sink.codec) return;
                    auto encoded = sink.codec->encode(v, sink);
                    if (encoded.isEmpty()) return;
                    for (auto* sw : m_sinkWindowPtrs) {
                        if (sw->id() != windowId) continue;
                        for (int i = 0; i < encoded.size(); ++i) {
                            quint16 mask = (sink.bit.has_value() && i == 0)
                                ? quint16(1u << *sink.bit)
                                : 0xFFFFu;
                            sw->stageRegister(sink.address + i,
                                              encoded.at(i), mask);
                        }
                        break;
                    }
                });
            }
        }
        return out;
    }

    void buildPollRanges(config::ConfigSchema const& schema,
                          DpById const&                byId) {
        for (auto const& pc : schema.pollRanges) {
            auto* t = transport(pc.transport);
            if (!t) continue;
            transport::ReadRequest req;
            req.table        = tableFromString(pc.table);
            req.startAddress = pc.startAddress;
            req.count        = pc.count;

            auto poll = std::make_unique<module::PollRange>(
                pc.moduleId, *t, req, pc.periodMs, pc.priority);
            wireBindings(*poll, schema, byId, pc.transport, req);

            auto* raw = poll.get();
            m_modules->registerModule(std::move(poll));
            m_pollRangePtrs.push_back(raw);
        }
    }

    void wireBindings(module::PollRange&            poll,
                       config::ConfigSchema const&    schema,
                       DpById const&                   byId,
                       QString const&                  transportId,
                       transport::ReadRequest const&   req) {
        for (auto const& dc : schema.datapoints) {
            if (!dc.hasSource) continue;
            if (dc.source.port != transportId) continue;
            if (tableFromString(dc.source.table) != req.table) continue;
            int const rc     = dp::registerCountFor(dc.type);
            int const offset = dc.source.address - req.startAddress;
            if (offset < 0 || offset + rc > req.count) continue;
            auto itDp = byId.find(dc.id);
            if (itDp == byId.end()) continue;

            auto codec = m_codecs->find(dc.source.codec.isEmpty()
                ? codec::BuiltinScalarCodec::idFor(dc.type)
                : dc.source.codec);
            if (!codec) continue;
            poll.bind(itDp->second, codec, offset);
        }
    }

private:
    QQmlContext*                                                m_qml;
    std::unique_ptr<log::Logger>                                m_logger;
    std::unique_ptr<qml::LogBridge>                             m_logBridge;
    std::unique_ptr<bus::EventBus>                              m_bus;
    std::unique_ptr<codec::CodecRegistry>                       m_codecs;
    std::unique_ptr<dp::DatapointRegistry>                      m_dps;
    std::unique_ptr<module::ModuleRegistry>                     m_modules;
    std::unique_ptr<plugin::PluginRegistry>                     m_plugins;
    std::map<QString, std::unique_ptr<transport::Transport>>    m_transports;
    std::vector<module::PollRange*>                             m_pollRangePtrs;
    std::vector<module::SinkWindow*>                            m_sinkWindowPtrs;
    std::map<QString, std::shared_ptr<dp::Datapoint>>           m_datapointById;
    QList<config::RouteConfig>                                  m_routes;
    std::unique_ptr<bus::Subscription>                          m_transportEventSub;
    std::unique_ptr<bus::Subscription>                          m_serverWriteSub;
    QTimer*                                                     m_statsTimer = nullptr;
    int                                                         m_statsIntervalMs = 1000;
};

std::unique_ptr<ICore> ICore::create(QQmlContext* qml) {
    return std::make_unique<CoreImpl>(qml);
}

namespace internal {
void pollAllOnce(ICore& core) {
    static_cast<CoreImpl&>(core).pollAllOnce();
}
void tickSinkWindowsOnce(ICore& core) {
    static_cast<CoreImpl&>(core).tickSinkWindowsOnce();
}
void publishSchedulerStatsOnce(ICore& core) {
    static_cast<CoreImpl&>(core).publishSchedulerStatsOnce();
}
} // namespace internal

} // namespace core
