// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/ICore.h"
#include "core/internal/Testing.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <QHash>
#include <QDateTime>
#ifdef CORE_HAS_QML
#include <QPointer>
#include <QQmlContext>
#endif
#include <QTimer>
#include <QtSerialBus/QModbusDataUnit>

#include "core/bus/EventBus.h"
#include "core/bus/BusEvents.h"
#include "core/bus/Subscription.h"
#include <QDir>
#include <QFileInfo>

#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/codec/LuaCodec.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/log/Logger.h"
#include "core/log/Sinks.h"
#ifdef CORE_HAS_QML
#include "core/qml/DatapointQmlBridge.h"
#include "core/qml/LogBridge.h"
#endif
#include "core/module/AckWatch.h"
#include "core/module/Command.h"
#include "core/module/Heartbeat.h"
#include "core/module/ModuleRegistry.h"
#include "core/module/PollRange.h"
#include "core/module/SinkWindow.h"
#include "core/plugin/Plugin.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"
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

quint16 stageMaskFor(dp::PortRef const& ref, int word, int wordCount) {
    if (wordCount != 1 || word != 0) return 0xFFFFu;
    if (ref.bit.has_value()) return quint16(1u << *ref.bit);
    return quint16((ref.mask << ref.shift) & 0xFFFFu);
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
    CoreImpl(QQmlContext* qml, bool installDefaultConsole)
        : m_qml(qml)
        , m_logger(std::make_unique<log::Logger>())
        , m_bus(std::make_unique<bus::EventBus>())
        , m_codecs(std::make_unique<codec::CodecRegistry>())
        , m_dps(std::make_unique<dp::DatapointRegistry>())
        , m_modules(std::make_unique<module::ModuleRegistry>())
        , m_plugins(std::make_unique<plugin::PluginRegistry>()) {
        m_codecs->loadBuiltins();
        // Built-in console sink so diagnostics surface out of the box; the app
        // adds file / DB sinks via logger().addSink(...). Apps that want to
        // filter or suppress console output pass installDefaultConsole=false
        // and register their own.
        if (installDefaultConsole) {
            m_logger->addSink(std::make_shared<log::ConsoleSink>());
        }
        installQmlBridges();
    }

    ~CoreImpl() override {
#ifdef CORE_HAS_QML
        if (m_qml) {
            m_qml->setContextProperty(QStringLiteral("dp"),
                                      static_cast<QObject*>(nullptr));
            m_qml->setContextProperty(QStringLiteral("log"),
                                      static_cast<QObject*>(nullptr));
        }
#endif
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
        stopMirrorPump();                      // no bridge mirror fires during teardown
        m_bridgeMirrors.clear();               // drop datapoint refs before the registry dies
        m_bridgeMirrorInFlight.clear();
        m_bridgeFwdSinks.clear();              // raw ptrs owned by m_modules
        m_bridges.clear();
        m_transportEventSub.reset();
        m_serverWriteSub.reset();
        joinConnectThreads();                  // no connect() in flight on a dying transport
        if (m_modules) m_modules->stopAll();   // stop ticks; keep modules alive
        m_transports.clear();                  // join worker threads → drain in-flight
        m_pollRangePtrs.clear();
        m_sinkWindowPtrs.clear();
        m_modules.reset();                     // safe: no completion can fire now
        // PortRegistry handlers capture plugin InPort references. Drop the bus
        // subscription and those handlers before destroying/unloading plugin
        // objects; Subscription destruction now drains concurrent dispatch.
        m_ports.reset();
        m_plugins.reset();
#ifdef CORE_HAS_QML
        m_dpBridge.reset();
#endif
        m_serverRouteBindings.clear();
        m_datapointById.clear();
        m_dps.reset();
        m_codecs.reset();
        m_bus.reset();
        // Logger last — subsystems above may log during teardown.
#ifdef CORE_HAS_QML
        m_logBridge.reset();
#endif
        if (m_logger) m_logger->stop();
        m_logger.reset();
    }

    std::expected<void, config::ValidationErrors>
    loadConfig(QString const& path) override {
        if (m_started) {
            return std::unexpected(config::ValidationErrors{
                {QStringLiteral("core"), QStringLiteral("loadConfig"),
                 QStringLiteral("configuration cannot be loaded while Core is running"),
                 -1}});
        }
        if (m_configLoaded) {
            return std::unexpected(config::ValidationErrors{
                {QStringLiteral("core"), QStringLiteral("loadConfig"),
                 QStringLiteral("configuration is already loaded; create a new Core instance to replace it"),
                 -1}});
        }
        if (m_configLoadPoisoned) {
            return std::unexpected(config::ValidationErrors{
                {QStringLiteral("core"), QStringLiteral("loadConfig"),
                 QStringLiteral("a previous runtime graph build failed; create a new Core instance"),
                 -1}});
        }
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

        // Resolve config-relative paths (e.g. lua codec scripts) against the
        // config file's directory.
        m_configDir = QFileInfo(path).absolutePath();
        auto codecErrors = registerCustomCodecs(*schema);
        if (!codecErrors.isEmpty()) {
            for (auto const& err : codecErrors) {
                m_logger->logf(log::LogLevel::Error,
                               QStringLiteral("config"), path, err.message);
            }
            return std::unexpected(std::move(codecErrors));
        }
        auto pluginErrors = loadPluginLibraries(*schema);
        if (!pluginErrors.isEmpty()) {
            m_plugins->unloadAll();
            for (auto const& err : pluginErrors) {
                m_logger->logf(log::LogLevel::Error,
                               QStringLiteral("config"), path, err.message);
            }
            return std::unexpected(std::move(pluginErrors));
        }
        if (!schema->meta.logLevel.isEmpty()) {
            m_logger->setThreshold(log::levelFromString(schema->meta.logLevel));
        }
        try {
            wireFromSchema(*schema);
        } catch (std::exception const& e) {
            m_ports.reset();
            m_plugins->unloadAll();
            m_configLoadPoisoned = true;
            QString const message = QStringLiteral("runtime graph initialization failed: %1")
                                        .arg(QString::fromUtf8(e.what()));
            m_logger->logf(log::LogLevel::Error,
                           QStringLiteral("config"), path, message);
            return std::unexpected(config::ValidationErrors{
                {QStringLiteral("core"), QStringLiteral("initialization"),
                 message, -1}});
        } catch (...) {
            m_ports.reset();
            m_plugins->unloadAll();
            m_configLoadPoisoned = true;
            QString const message = QStringLiteral(
                "runtime graph initialization failed with an unknown exception");
            m_logger->logf(log::LogLevel::Error,
                           QStringLiteral("config"), path, message);
            return std::unexpected(config::ValidationErrors{
                {QStringLiteral("core"), QStringLiteral("initialization"),
                 message, -1}});
        }
        m_configLoaded = true;
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

    void setServerForwardEnabled(QString const& serverTransportId, bool enabled) override {
        bool wasEnabled;
        {
            std::lock_guard lk(m_forwardMtx);
            wasEnabled = m_forwardEnabled.value(serverTransportId, true);
            m_forwardEnabled[serverTransportId] = enabled;
        }
        // 关闭瞬间(true→false):把该 server 桥接的转发区在程序内置 0,取消尚未写入
        // PLC 的操作箱指令(中性/停机)。只在边沿触发,避免每次调用重复 stage。
        if (wasEnabled && !enabled) {
            zeroBridgeForward(serverTransportId);
        }
    }

    bool serverForwardEnabled(QString const& serverTransportId) const override {
        std::lock_guard lk(m_forwardMtx);
        return m_forwardEnabled.value(serverTransportId, true);
    }

    void start() override {
        if (m_started) return;   // idempotent while already running
        if (!m_configLoaded) {
            m_logger->logf(log::LogLevel::Error,
                           QStringLiteral("core"), QStringLiteral("ICore"),
                           QStringLiteral("start rejected: no valid configuration is loaded"));
            return;
        }
        m_started = true;
        installEventWiring();
        // Connect transports in PARALLEL, off the calling (GUI) thread: a slow
        // or unreachable transport (e.g. a wrong MQTT / OPC UA endpoint) must
        // not stall start() for its whole connect timeout. The threads are
        // joined at stop()/teardown. Modules start polling immediately; reads
        // before a connection completes simply report "not connected".
        for (auto& [id, t] : m_transports) {
            transport::Transport* tp = t.get();
            m_connectThreads.emplace_back([this, tp, id]() {
                auto const connected = tp->connect();
                if (!connected) {
                    m_logger->logf(log::LogLevel::Error,
                                   QStringLiteral("transport"), id,
                                   QStringLiteral("initial connect failed: %1")
                                       .arg(connected.error()));
                }
            });
        }
        m_modules->startAll();
        startStatsPump();
        startMirrorPump();
        m_bus->publish(bus::CoreReady{});
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("started"));
    }

    void stop() override {
        if (!m_started) return;
        m_started = false;
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("stopping"));
        joinConnectThreads();   // no connect in flight before we disconnect
        m_bus->publish(bus::CoreStopping{});
        stopStatsPump();
        stopMirrorPump();
        m_modules->stopAll();
        for (auto& [id, t] : m_transports) t->disconnect();
        m_logger->flush();
    }

    void joinConnectThreads() {
        for (auto& th : m_connectThreads) {
            if (th.joinable()) th.join();
        }
        m_connectThreads.clear();
    }

    void pollAllOnce() {
        for (auto* poll : m_pollRangePtrs) poll->pollOnce();
    }

    void tickSinkWindowsOnce() {
        for (auto* sw : m_sinkWindowPtrs) sw->onTick();
    }

    void publishSchedulerStatsOnce() {
        for (auto& [id, t] : m_transports) {
            auto const stats = t->scheduler().stats();
            m_bus->publish(bus::SchedulerStatsEvent{id, stats});
            auto const previous = m_lastCircuitStates.value(
                id, sched::CircuitState::Closed);
            if (stats.circuitState != previous) {
                bus::TransportEventKind kind {};
                switch (stats.circuitState) {
                    case sched::CircuitState::Open:
                        kind = bus::TransportEventKind::CircuitOpened;
                        break;
                    case sched::CircuitState::HalfOpen:
                        kind = bus::TransportEventKind::CircuitHalfOpen;
                        break;
                    case sched::CircuitState::Closed:
                        kind = bus::TransportEventKind::CircuitClosed;
                        break;
                }
                m_bus->publish(bus::TransportEvent{id, kind, {}});
            }
            m_lastCircuitStates[id] = stats.circuitState;
        }
    }

    void setSchedulerStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }

private:
    void registerModule(std::unique_ptr<module::FunctionalModule> mod) {
        QString const id = mod ? mod->id() : QString{};
        if (!m_modules->registerModule(std::move(mod))) {
            throw std::runtime_error(
                QStringLiteral("failed to register duplicate/invalid module '%1'")
                    .arg(id).toStdString());
        }
    }

    void wireFromSchema(config::ConfigSchema const& schema) {
        buildTransports(schema);
        // Sink windows are needed while datapoints are built so an implicit
        // sink (port/table/address without `window`) can be resolved once and
        // QML writes cannot silently end up with no writer.
        buildSinkWindows(schema);
        auto byId = buildDatapoints(schema);
        buildPollRanges(schema, byId);
        buildServerRoutes(schema);
        buildHeartbeats(schema);
        buildAckWatches(schema);
        buildCommands(schema);
        buildBridges(schema);
        initializePlugins();
    }

    // Load every declared DLL before mutating the runtime object graph. A bad
    // path/export is a configuration failure, not a partially functional Core.
    config::ValidationErrors
    loadPluginLibraries(config::ConfigSchema const& schema) {
        config::ValidationErrors errs;
        for (int i = 0; i < schema.plugins.size(); ++i) {
            auto const& pc = schema.plugins[i];
            QString dll = pc.dllPath;
            if (QFileInfo(dll).isRelative() && !m_configDir.isEmpty())
                dll = QDir(m_configDir).filePath(dll);
            if (!m_plugins->load(dll)) {
                errs.push_back({QStringLiteral("plugin[%1]").arg(i),
                                QStringLiteral("dll"),
                                QStringLiteral("failed to load '%1': %2")
                                    .arg(dll, m_plugins->lastError()),
                                -1});
            } else if (!pc.name.isEmpty()) {
                auto const loaded = m_plugins->all();
                auto* const plugin = loaded.isEmpty() ? nullptr : loaded.back();
                QString actualName = QStringLiteral("<none>");
                bool nameOk = false;
                try {
                    if (plugin) {
                        actualName = plugin->name();
                        nameOk = actualName == pc.name;
                    }
                } catch (std::exception const& e) {
                    actualName = QStringLiteral("<name() threw: %1>")
                                     .arg(QString::fromUtf8(e.what()));
                } catch (...) {
                    actualName = QStringLiteral("<name() threw>");
                }
                if (!nameOk) {
                    errs.push_back({QStringLiteral("plugin[%1]").arg(i),
                                    QStringLiteral("name"),
                                    QStringLiteral("declared plugin name '%1' does not match loaded plugin '%2'")
                                        .arg(pc.name, actualName),
                                    -1});
                }
            }
        }
        return errs;
    }

    // Once datapoints exist, let preloaded plugins bind ports and initialize.
    void initializePlugins() {
        if (m_plugins->all().isEmpty()) return;
        m_ports = std::make_unique<plugin::PortRegistry>(*m_dps, *m_bus);
        m_plugins->registerAllPorts(*m_ports);
        for (auto* p : m_plugins->all()) p->onInitialized();
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

    void installQmlBridges() {
#ifdef CORE_HAS_QML
        if (!m_qml) return;
        m_dpBridge = std::make_unique<qml::DatapointQmlBridge>(*m_dps);
        m_logBridge = std::make_unique<qml::LogBridge>(*m_logger);
        m_qml->setContextProperty(QStringLiteral("dp"), m_dpBridge.get());
        m_qml->setContextProperty(QStringLiteral("log"), m_logBridge.get());
#endif
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
                        QStringLiteral("ok"), {},
                        QStringLiteral("audit"),
                        QStringLiteral("server-write:") + QString::number(e.startAddress)});
                    if (!serverForwardEnabled(e.transportId)) return;   // 业务闸门:不转发
                    routeServerWrite(e);
                    forwardBridges(e);
                }));
    }

    void logTransportEvent(bus::TransportEvent const& e) {
        // An explicit Core::stop() disconnect is expected lifecycle noise, not
        // a connectivity warning. Unexpected runtime drops still arrive while
        // m_started is true and remain visible.
        if (!m_started && e.kind == bus::TransportEventKind::Disconnected) return;
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
        auto const it = m_serverRouteBindings.constFind(e.transportId);
        if (it == m_serverRouteBindings.constEnd()) return;
        for (auto const& binding : it.value()) {
            if (binding.source.table != e.table) continue;
            int const offset = binding.source.address - e.startAddress;
            if (offset < 0
                || offset + binding.sourceRegisterCount > e.values.size()) {
                continue;
            }
            QVariant decoded;
            QList<quint16> encoded;
            try {
                decoded = binding.source.codec->decode(
                    e.values.mid(offset, binding.sourceRegisterCount),
                    binding.source);
                if (!decoded.isValid()) {
                    binding.sourceDp->setState(dp::DpState::Error);
                    binding.targetDp->setState(dp::DpState::Error);
                    continue;
                }
                binding.sourceDp->setValue(decoded);
                encoded = binding.sink.codec->encode(decoded, binding.sink);
            } catch (...) {
                binding.sourceDp->setState(dp::DpState::Error);
                binding.targetDp->setState(dp::DpState::Error);
                continue;
            }
            if (encoded.isEmpty()) {
                binding.targetDp->setState(dp::DpState::Error);
                continue;
            }
            for (int word = 0; word < encoded.size(); ++word) {
                binding.target->stageRegister(
                    binding.sink.address + word, encoded.at(word),
                    stageMaskFor(binding.sink, word, encoded.size()));
            }
        }
    }

    void buildServerRoutes(config::ConfigSchema const& schema) {
        m_serverRouteBindings.clear();
        for (auto const& route : schema.routes) {
            auto const fromIt = m_datapointById.find(route.from);
            auto const toIt   = m_datapointById.find(route.to);
            if (fromIt == m_datapointById.end()
             || toIt == m_datapointById.end()) continue;

            auto const source = fromIt->second->source();
            auto const sink   = toIt->second->sink();
            if (!source || !sink || !source->codec || !sink->codec) continue;

            module::SinkWindow* target = nullptr;
            for (auto* sw : m_sinkWindowPtrs) {
                if (!sink->window.isEmpty() && sw->id() == sink->window) {
                    target = sw;
                    break;
                }
            }
            if (!target && !sink->transport.isEmpty()) {
                for (auto* sw : m_sinkWindowPtrs) {
                    if (sw->transportId() != sink->transport) continue;
                    if (sink->address < sw->startAddress()
                     || sink->address >= sw->startAddress() + sw->size()) continue;
                    target = sw;
                    break;
                }
            }
            if (!target) continue;   // validation normally prevents this

            m_serverRouteBindings[source->transport].append(ServerRouteBinding{
                *source, dp::registerCountFor(fromIt->second->type()),
                fromIt->second, toIt->second, target, *sink});
        }
    }

    // ——— 整段桥接(替代旧 ModbusServer 中继) ———
    void buildBridges(config::ConfigSchema const& schema) {
        m_bridges = schema.bridges;
        m_bridgeMirrors.assign(size_t(m_bridges.size()), {});
        m_bridgeMirrorSnapshots.assign(size_t(m_bridges.size()), {});
        m_bridgeMirrorLastAt.assign(size_t(m_bridges.size()), {});
        m_bridgeMirrorInFlight.clear();
        m_bridgeMirrorInFlight.reserve(size_t(m_bridges.size()));
        for (int i = 0; i < m_bridges.size(); ++i) {
            m_bridgeMirrorInFlight.push_back(
                std::make_shared<std::atomic_bool>(false));
        }
        m_bridgeFwdSinks.assign(size_t(m_bridges.size()), nullptr);
        for (int i = 0; i < m_bridges.size(); ++i) {
            auto const& b = m_bridges[i];
            auto& list = m_bridgeMirrors[size_t(i)];
            m_bridgeMirrorSnapshots[size_t(i)].fill(0, b.mirrorCount);
            for (auto const& dp : m_dps->all()) {
                auto const src = dp->source();
                if (!src.has_value()) continue;
                if (src->transport != b.plc) continue;
                if (src->table != QModbusDataUnit::HoldingRegisters) continue;
                int const a = src->address;
                if (a < b.mirrorStart || a >= b.mirrorStart + b.mirrorCount) continue;
                list.push_back(BridgeMirrorBinding{a, dp, *src});
            }
            // 转发走 PLC 侧的 SinkWindow(同 route 路径):线程安全地 stageRegister,
            // 由 TickDriver 在生命周期线程带重试/coalesce 地刷到 PLC —— 避免单次
            // submitAsync 在调度器繁忙时被丢弃。
            if (b.writeCount > 0) {
                if (auto* plc = transport(b.plc)) {
                    module::SinkWindow::Config cfg;
                    cfg.moduleId = QStringLiteral("bridge.fwd.%1.%2")
                        .arg(b.server).arg(i);
                    cfg.table          = QModbusDataUnit::HoldingRegisters;
                    cfg.startAddress   = b.writeStart - b.offset;
                    cfg.size           = b.writeCount;
                    cfg.priority       = sched::Priority::High;
                    cfg.debounceMs     = 0;
                    cfg.keepAlivePeriodMs = 0;
                    auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *plc);
                    auto* raw = sw.get();
                    registerModule(std::move(sw));
                    m_bridgeFwdSinks[size_t(i)] = raw;
                    m_sinkWindowPtrs.push_back(raw);
                }
            }
        }
    }

    // 操作箱写 server → 写区子段 stage 到 PLC 侧 SinkWindow(server 地址 - offset)。
    // stageRegister 线程安全,可直接在 server transport 线程调用;刷写由 TickDriver 做。
    void forwardBridges(bus::ServerWriteEvent const& e) {
        if (e.table != QModbusDataUnit::HoldingRegisters) return;
        for (int i = 0; i < m_bridges.size(); ++i) {
            auto const& b = m_bridges[i];
            if (b.server != e.transportId) continue;
            auto* sink = m_bridgeFwdSinks[size_t(i)];
            if (!sink) continue;
            int const eStart = e.startAddress;
            int const eEnd   = e.startAddress + int(e.values.size());
            int const s  = std::max(eStart, b.writeStart);
            int const en = std::min(eEnd, b.writeStart + b.writeCount);
            for (int a = s; a < en; ++a) {
                sink->stageRegister(a - b.offset, e.values.at(a - eStart));
            }
        }
    }

    // 关闭转发瞬间:把该 server 桥接的整个转发写区在 PLC 侧 SinkWindow 内置 0,
    // 由 TickDriver 下发 —— 取消尚未写入 PLC 的操作箱指令。
    void zeroBridgeForward(QString const& server) {
        for (int i = 0; i < m_bridges.size(); ++i) {
            auto const& b = m_bridges[i];
            if (b.server != server) continue;
            auto* sink = m_bridgeFwdSinks[size_t(i)];
            if (!sink) continue;
            for (int a = b.writeStart; a < b.writeStart + b.writeCount; ++a) {
                sink->stageRegister(a - b.offset, 0);
            }
            // stageRegister intentionally ignores unchanged values. Disabling
            // forwarding is a safety edge, however: force a physical zero
            // snapshot even when our local snapshot was already zero, because
            // the PLC may have been changed by another master or a prior run.
            sink->forceFlush();
        }
    }

public:
    // 周期把 PLC 读区数据整段镜像回 server 自己的寄存器表(操作箱即可读到)。
    // 也是测试入口(internal::mirrorBridgesOnce)。
    void mirrorBridgesOnce() {
        mirrorBridges(false);
    }

private:
    void mirrorBridges(bool respectCadence) {
        auto const now = std::chrono::steady_clock::now();
        for (int i = 0; i < m_bridges.size(); ++i) {
            auto const& b = m_bridges[i];
            if (b.mirrorCount <= 0) continue;
            auto& lastAt = m_bridgeMirrorLastAt[size_t(i)];
            if (respectCadence && lastAt.time_since_epoch().count() > 0
                && now - lastAt < std::chrono::milliseconds(b.mirrorPeriodMs)) {
                continue;
            }
            auto* server = transport(b.server);
            if (!server) continue;
            auto& values = m_bridgeMirrorSnapshots[size_t(i)];
            for (auto const& binding : m_bridgeMirrors[size_t(i)]) {
                if (binding.datapoint->state() != dp::DpState::Ok
                    || !binding.source.codec) {
                    continue; // retain the last good raw mirror value
                }
                QList<quint16> encoded;
                try {
                    encoded = binding.source.codec->encode(
                        binding.datapoint->value(), binding.source);
                } catch (...) {
                    continue;
                }
                int const first = binding.address - b.mirrorStart;
                for (int word = 0; word < encoded.size(); ++word) {
                    int const idx = first + word;
                    if (idx >= 0 && idx < values.size()) {
                        values[idx] = encoded.at(word);
                    }
                }
            }
            transport::WriteBatch batch;
            batch.table        = QModbusDataUnit::HoldingRegisters;
            batch.startAddress = b.mirrorStart + b.offset;
            batch.values       = values;
            sched::RequestTag tag;
            tag.moduleId = QStringLiteral("bridge.mirror.%1.%2")
                .arg(b.server).arg(i);
            tag.priority = sched::Priority::Low;
            auto const inFlight = m_bridgeMirrorInFlight[size_t(i)];
            bool expected = false;
            if (!inFlight->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                continue;
            }
            auto const submitted = server->scheduler().submitAsync(
                tag, [server, batch, inFlight](sched::AsyncDone done) {
                try {
                    server->writeAsync(batch,
                        [done = std::move(done), inFlight](transport::WriteResult w) mutable {
                            inFlight->store(false, std::memory_order_release);
                            done(w.ok);
                        });
                } catch (...) {
                    inFlight->store(false, std::memory_order_release);
                    throw;
                }
            });
            if (submitted.kind == sched::ResultKind::Ok) {
                lastAt = now;
            } else {
                inFlight->store(false, std::memory_order_release);
            }
        }
    }

    void startMirrorPump() {
        if (m_mirrorTimer) return;
        int period = std::numeric_limits<int>::max();
        bool any = false;
        for (auto const& b : m_bridges) {
            if (b.mirrorCount > 0) { any = true; period = std::min(period, std::max(20, b.mirrorPeriodMs)); }
        }
        if (!any) return;
        m_mirrorTimer = new QTimer(m_bus.get());
        m_mirrorTimer->setInterval(period);
        m_mirrorTimer->setSingleShot(false);
        QObject::connect(m_mirrorTimer, &QTimer::timeout, m_bus.get(),
            [this]() { mirrorBridges(true); });
        m_mirrorTimer->start();
    }

    void stopMirrorPump() {
        if (!m_mirrorTimer) return;
        m_mirrorTimer->stop();
        m_mirrorTimer->deleteLater();
        m_mirrorTimer = nullptr;
    }

    config::ValidationErrors
    registerCustomCodecs(config::ConfigSchema const& schema) {
        config::ValidationErrors errs;
        for (int i = 0; i < schema.codecs.size(); ++i) {
            auto const& cc = schema.codecs[i];
            if (cc.kind == "enum_u16") {
                std::unordered_map<quint16, QString> map;
                for (auto it = cc.map.constBegin(); it != cc.map.constEnd(); ++it) {
                    bool ok = false;
                    uint const raw = it.key().toUInt(&ok);
                    if (!ok || raw > 65535) continue;   // schema validation caught it
                    map.emplace(quint16(raw), it.value().toString());
                }
                m_codecs->registerCodec(
                    std::make_shared<codec::EnumU16Codec>(cc.id, std::move(map)));
            } else if (cc.kind == QStringLiteral("lua")) {
                QString script = cc.script;
                if (QFileInfo(script).isRelative() && !m_configDir.isEmpty())
                    script = QDir(m_configDir).filePath(script);
                QString err;
                auto lc = codec::LuaCodec::fromFile(cc.id, script, cc.arg, &err);
                if (lc) {
                    m_codecs->registerCodec(std::move(lc));
                } else {
                    errs.push_back({QStringLiteral("codec[%1]").arg(i),
                                    QStringLiteral("script"),
                                    err.isEmpty()
                                        ? QStringLiteral("lua codec load failed")
                                        : err,
                                    -1});
                }
            }
        }
        return errs;
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
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
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
                cfg.connectTimeoutMs      = tc.connectTimeoutMs;
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
            cfg.initial           = sc.initial;
            auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *t);
            auto* raw = sw.get();
            registerModule(std::move(sw));
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
            registerModule(
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
            registerModule(
                std::make_unique<module::AckWatch>(std::move(cfg), *m_bus,
                                                   m_dps.get()));
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
            registerModule(
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
                } else {
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
                auto sinkCodec = dc.sink.codec.isEmpty()
                    ? m_codecs->find(codec::BuiltinScalarCodec::idFor(dc.type))
                    : m_codecs->find(dc.sink.codec);
                auto sinkRef = makePortRef(dc.sink, std::move(sinkCodec));
                config::SinkWindowConfig const* owner = nullptr;
                if (!dc.sink.window.isEmpty()) {
                    for (auto const& sw : schema.sinkWindows) {
                        if (sw.moduleId == dc.sink.window) {
                            owner = &sw;
                            break;
                        }
                    }
                } else {
                    int const words = std::max(1, dp::registerCountFor(dc.type));
                    for (auto const& sw : schema.sinkWindows) {
                        if (sw.transport != dc.sink.port
                            || tableFromString(sw.table) != sinkRef.table
                            || dc.sink.address < sw.startAddress
                            || qint64(dc.sink.address) + words
                                > qint64(sw.startAddress) + sw.size) {
                            continue;
                        }
                        owner = &sw;
                        break;
                    }
                }
                if (owner) {
                    sinkRef.window    = owner->moduleId;
                    sinkRef.transport = owner->transport;
                    sinkRef.table     = tableFromString(owner->table);
                }
                spec.sink = std::move(sinkRef);
            }
            spec.uiBinding  = dc.ui;
            spec.persistTag = dc.persist;

            auto datapoint = std::make_shared<dp::Datapoint>(std::move(spec));
            if (!m_dps->registerDp(datapoint)) {
                throw std::runtime_error(
                    QStringLiteral("failed to register duplicate/invalid datapoint '%1'")
                        .arg(dc.id).toStdString());
            }
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
            QObject::connect(datapoint.get(), &dp::Datapoint::stateChanged,
                m_bus.get(), [this, weak] {
                    auto sp = weak.lock();
                    if (!sp) return;
                    m_bus->publish(bus::DpStateChanged{
                        sp->id(), sp->value(), int(sp->state()),
                        QDateTime::currentDateTime()});
                });

            // For Command / Bidirectional datapoints with a SinkWindow sink,
            // wire a writer that encodes the value through the codec then
            // stages into the named SinkWindow. QML calls `dp.write(v)` and
            // the new value lands on the PLC at the next flush tick.
            auto const sink = datapoint->sink();
            if ((datapoint->kind() == dp::Kind::Command
                 || datapoint->kind() == dp::Kind::Bidirectional)
                && sink.has_value()
                && !sink->window.isEmpty()) {
                QString const windowId = sink->window;
                datapoint->setWriter([this, weak, windowId](QVariant v) {
                    auto sp = weak.lock();
                    if (!sp) return;
                    auto const sinkRef = sp->sink();
                    if (!sinkRef.has_value()) {
                        sp->setState(dp::DpState::Error);
                        return;
                    }
                    auto sink = *sinkRef;
                    if (!sink.codec) {
                        sp->setState(dp::DpState::Error);
                        return;
                    }
                    QList<quint16> encoded;
                    try {
                        encoded = sink.codec->encode(v, sink);
                    } catch (...) {
                        sp->setState(dp::DpState::Error);
                        return;
                    }
                    if (encoded.isEmpty()) {
                        sp->setState(dp::DpState::Error);
                        return;
                    }
                    bool staged = false;
                    for (auto* sw : m_sinkWindowPtrs) {
                        if (sw->id() != windowId) continue;
                        for (int i = 0; i < encoded.size(); ++i) {
                            quint16 const mask = stageMaskFor(
                                sink, i, encoded.size());
                            sw->stageRegister(sink.address + i,
                                              encoded.at(i), mask);
                        }
                        staged = true;
                        break;
                    }
                    if (!staged) sp->setState(dp::DpState::Error);
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
            registerModule(std::move(poll));
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
    struct ServerRouteBinding {
        dp::PortRef                   source;
        int                           sourceRegisterCount;
        std::shared_ptr<dp::Datapoint> sourceDp;
        std::shared_ptr<dp::Datapoint> targetDp;
        module::SinkWindow*           target;
        dp::PortRef                   sink;
    };

    struct BridgeMirrorBinding {
        int                           address;
        std::shared_ptr<dp::Datapoint> datapoint;
        dp::PortRef                   source;
    };

#ifdef CORE_HAS_QML
    QPointer<QQmlContext>                                       m_qml;
#else
    QQmlContext*                                                m_qml;
#endif
    QString                                                     m_configDir;
    std::unique_ptr<log::Logger>                                m_logger;
#ifdef CORE_HAS_QML
    std::unique_ptr<qml::DatapointQmlBridge>                    m_dpBridge;
    std::unique_ptr<qml::LogBridge>                             m_logBridge;
#endif
    std::unique_ptr<bus::EventBus>                              m_bus;
    std::unique_ptr<codec::CodecRegistry>                       m_codecs;
    std::unique_ptr<dp::DatapointRegistry>                      m_dps;
    std::unique_ptr<module::ModuleRegistry>                     m_modules;
    std::unique_ptr<plugin::PluginRegistry>                     m_plugins;
    std::unique_ptr<plugin::PortRegistry>                       m_ports;
    std::map<QString, std::unique_ptr<transport::Transport>>    m_transports;
    std::vector<std::thread>                                    m_connectThreads;
    std::atomic_bool                                            m_started{false};
    bool                                                        m_configLoaded = false;
    bool                                                        m_configLoadPoisoned = false;
    std::vector<module::PollRange*>                             m_pollRangePtrs;
    std::vector<module::SinkWindow*>                            m_sinkWindowPtrs;
    std::map<QString, std::shared_ptr<dp::Datapoint>>           m_datapointById;
    QHash<QString, QList<ServerRouteBinding>>                   m_serverRouteBindings;
    QHash<QString, sched::CircuitState>                         m_lastCircuitStates;
    QList<config::BridgeConfig>                                 m_bridges;
    std::vector<std::vector<BridgeMirrorBinding>>               m_bridgeMirrors;
    std::vector<QList<quint16>>                                 m_bridgeMirrorSnapshots;
    std::vector<std::chrono::steady_clock::time_point>          m_bridgeMirrorLastAt;
    std::vector<std::shared_ptr<std::atomic_bool>>               m_bridgeMirrorInFlight;
    std::vector<module::SinkWindow*>                            m_bridgeFwdSinks;
    mutable std::mutex                                          m_forwardMtx;
    QHash<QString, bool>                                        m_forwardEnabled;   // server id → 转发使能(默认 true)
    QTimer*                                                     m_mirrorTimer = nullptr;
    std::unique_ptr<bus::Subscription>                          m_transportEventSub;
    std::unique_ptr<bus::Subscription>                          m_serverWriteSub;
    QTimer*                                                     m_statsTimer = nullptr;
    int                                                         m_statsIntervalMs = 1000;
};

std::unique_ptr<ICore> ICore::create(QQmlContext* qml, bool installDefaultConsole) {
    return std::make_unique<CoreImpl>(qml, installDefaultConsole);
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
void mirrorBridgesOnce(ICore& core) {
    static_cast<CoreImpl&>(core).mirrorBridgesOnce();
}
} // namespace internal

} // namespace core
