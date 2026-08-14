// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/ICore.h"
#include "core/internal/Testing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QHash>
#ifdef CORE_HAS_QML
#include <QQmlContext>
#endif
#include <QThread>
#include <QTimer>

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
#include "core/dp/ValueQt.h"
#include "core/log/Logger.h"
#include "core/log/Sinks.h"
#ifdef CORE_HAS_QML
#include "core/qml/CoreQml.h"
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

// Marshal the Qt-free config IR strings into the QString-based runtime types the
// Qt HMI assembly still uses. The config schema (core::config) is Qt-free; this
// is the adapter edge.
inline QString qs(std::string const& s) { return QString::fromStdString(s); }

core::RegisterTable tableFromString(QString const& s) {
    static const QHash<QString, core::RegisterTable> map = {
        {"HR",                core::RegisterTable::HoldingRegister},
        {"HoldingRegisters",  core::RegisterTable::HoldingRegister},
        {"IR",                core::RegisterTable::InputRegister},
        {"InputRegisters",    core::RegisterTable::InputRegister},
        {"Coil",              core::RegisterTable::Coil},
        {"Coils",             core::RegisterTable::Coil},
        {"DI",                core::RegisterTable::DiscreteInput},
        {"DiscreteInputs",    core::RegisterTable::DiscreteInput},
    };
    return map.value(s, core::RegisterTable::HoldingRegister);
}

dp::WordOrder wordOrderFromString(QString const& s) {
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    return dp::WordOrder::ABCD;
}

dp::Kind kindFromString(std::string const& s) {
    if (s == "Command")       return dp::Kind::Command;
    if (s == "Bidirectional") return dp::Kind::Bidirectional;
    return dp::Kind::Status;
}

dp::Value makeDisconnectValue(dp::ScalarType type, double value) {
    switch (type) {
        case dp::ScalarType::Bool:
            return value != 0.0;
        case dp::ScalarType::F32:
        case dp::ScalarType::F64:
            return value;
        case dp::ScalarType::S16:
        case dp::ScalarType::S32:
        case dp::ScalarType::S64:
            return std::int64_t(value);
        case dp::ScalarType::String:
            return std::string{};
        default:
            return std::uint64_t(value);
    }
}

dp::PortRef makePortRef(config::PortRefConfig const& pc,
                         std::shared_ptr<codec::Codec> codec) {
    dp::PortRef p;
    p.transport = pc.port;
    p.table     = tableFromString(qs(pc.table));
    p.address   = pc.address;
    if (pc.bit >= 0) p.bit = pc.bit;
    p.wordOrder = wordOrderFromString(qs(pc.wordOrder));
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
    explicit CoreImpl(bool installDefaultConsole)
        : m_logger(std::make_unique<log::Logger>())
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
        // QML `log` bridge is opt-in via wireQml() (Qt layer); not wired here.
    }

#ifdef CORE_HAS_QML
    // Expose the `log` bridge (owned by Core) on a QML context.
    void wireQml(QQmlContext* qml) {
        if (!qml) return;
        m_logBridge = std::make_unique<qml::LogBridge>(*m_logger);
        qml->setContextProperty(QStringLiteral("log"), m_logBridge.get());
    }
#endif

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
        stopMirrorPump();                      // no bridge mirror fires during teardown
        m_bridgeFwdSinks.clear();              // raw ptrs owned by m_modules
        m_transportEventSub.reset();
        m_transportStateSub.reset();
        m_peerSessionSub.reset();
        m_pollRangeCompletedSub.reset();
        m_serverWriteSub.reset();
        joinConnectThreads();                  // no connect() in flight on a dying transport
        if (m_modules) m_modules->stopAll();   // stop ticks; keep modules alive
        m_transports.clear();                  // join worker threads → drain in-flight
        m_pollRangePtrs.clear();
        m_sinkWindowPtrs.clear();
        m_modules.reset();                     // safe: no completion can fire now
        m_bridgeMirrorStates.clear();
        m_bridges.clear();
        m_plugins.reset();                     // destroy plugins (and their port emitters) first
        m_ports.reset();                       // then the port registry (drops its bus sub)
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
    loadConfig(std::string const& path) override {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_started.load(std::memory_order_acquire)) {
            return std::unexpected(config::ValidationErrors{
                {"core", "loadConfig",
                 "configuration cannot be loaded while Core is running", -1}});
        }
        if (m_configLoaded) {
            return std::unexpected(config::ValidationErrors{
                {"core", "loadConfig",
                 "configuration is already loaded; use reloadConfig()", -1}});
        }
        config::ConfigLoader loader;
        auto schema = loader.loadFromToml(path);
        if (!schema.has_value()) {
            for (auto const& err : schema.error()) {
                m_logger->logf(log::LogLevel::Error,
                               "config", path,
                               err.message);
            }
            return std::unexpected(schema.error());
        }

        auto built = buildRuntimeFromSchema(*schema, path);
        if (!built.has_value()) {
            destroyRuntimeGraph();
            initializeRuntimeGraph();
            return built;
        }
        m_configLoaded = true;
        m_activeSchema = *schema;
        m_activeConfigPath =
            QFileInfo(QString::fromStdString(path)).absoluteFilePath()
                .toStdString();
        refreshSnapshotCache();
        m_logger->logf(log::LogLevel::Info, "config", path,
                       "loaded",
                       {{"transports", std::int64_t(schema->transports.size())},
                        {"datapoints", std::int64_t(schema->datapoints.size())}});
        return {};
    }

    std::expected<void, config::ValidationErrors>
    reloadConfig(std::string const& path) override {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_reloadInProgress.exchange(true, std::memory_order_acq_rel)) {
            return std::unexpected(config::ValidationErrors{
                {"core", "reloadConfig",
                 "another configuration reload is already in progress", -1}});
        }
        struct ReloadFlagReset {
            std::atomic_bool& flag;
            ~ReloadFlagReset() {
                flag.store(false, std::memory_order_release);
            }
        } resetReloadFlag{m_reloadInProgress};

        if (!m_configLoaded || !m_activeSchema.has_value()) {
            return std::unexpected(config::ValidationErrors{
                {"core", "reloadConfig",
                 "no active configuration to reload", -1}});
        }

        std::string const absolutePath =
            QFileInfo(QString::fromStdString(path)).absoluteFilePath()
                .toStdString();
        m_bus->publish(bus::ConfigReloadStarted{absolutePath});

        config::ConfigLoader loader;
        auto candidateSchema = loader.loadFromToml(absolutePath);
        if (!candidateSchema.has_value()) {
            auto const reason = summarizeErrors(candidateSchema.error());
            m_bus->publish(
                bus::ConfigReloadFailed{absolutePath, reason});
            return std::unexpected(candidateSchema.error());
        }

        bool const wasRunning =
            m_started.load(std::memory_order_acquire);
        auto const previousSchema = *m_activeSchema;
        auto const previousPath = m_activeConfigPath;
        if (wasRunning) stop();
        destroyRuntimeGraph();
        initializeRuntimeGraph();

        auto built = buildRuntimeFromSchema(*candidateSchema, absolutePath);
        if (built.has_value()) {
            m_configLoaded = true;
            m_activeSchema = *candidateSchema;
            m_activeConfigPath = absolutePath;
            refreshSnapshotCache();
            for (auto const& transportConfig
                 : candidateSchema->transports) {
                if (transportConfig.kind
                    == transport::TransportKind::ModbusTcpServer) {
                    setServerForwardEnabled(transportConfig.id, false);
                }
            }
            ++m_datapointGeneration;
            if (wasRunning) start();
            m_bus->publish(
                bus::DatapointModelRebuilt{m_datapointGeneration});
            m_bus->publish(bus::ConfigReloadSucceeded{absolutePath});
            return {};
        }

        auto reloadErrors = built.error();
        destroyRuntimeGraph();
        initializeRuntimeGraph();
        auto rollback = buildRuntimeFromSchema(previousSchema, previousPath);
        if (rollback.has_value()) {
            m_configLoaded = true;
            m_activeSchema = previousSchema;
            m_activeConfigPath = previousPath;
            refreshSnapshotCache();
            if (wasRunning) start();
        } else {
            reloadErrors.insert(reloadErrors.end(),
                                rollback.error().begin(),
                                rollback.error().end());
        }
        auto const reason = summarizeErrors(reloadErrors);
        m_bus->publish(bus::ConfigReloadFailed{absolutePath, reason});
        return std::unexpected(std::move(reloadErrors));
    }

    bus::EventBus&            bus()        override { return *m_bus; }
    dp::DatapointRegistry&    datapoints() override { return *m_dps; }
    codec::CodecRegistry&     codecs()     override { return *m_codecs; }
    module::ModuleRegistry&   modules()    override { return *m_modules; }
    plugin::PluginRegistry&   plugins()    override { return *m_plugins; }
    log::Logger&              logger()     override { return *m_logger; }

    transport::Transport* transport(std::string const& id) const override {
        auto it = m_transports.find(id);
        return it == m_transports.end() ? nullptr : it->second.get();
    }

    std::vector<std::string> transportIds() const override {
        std::vector<std::string> ids;
        ids.reserve(m_transports.size());
        for (auto const& [id, t] : m_transports) ids.push_back(id);
        return ids;
    }

    transport::TransportStatus
    transportStatus(std::string const& id) const override {
        {
            std::lock_guard lock(m_snapshotMutex);
            auto const it = m_transportStatusCache.find(id);
            if (it != m_transportStatusCache.end()) return it->second;
        }
        transport::TransportStatus missing;
        missing.transportId = id;
        missing.state = transport::ConnectionState::Error;
        missing.errorMessage = "transport not found";
        missing.changedAt = std::chrono::system_clock::now();
        return missing;
    }

    std::vector<transport::TransportStatus>
    transportStatuses() const override {
        std::lock_guard lock(m_snapshotMutex);
        std::vector<transport::TransportStatus> snapshots;
        snapshots.reserve(m_transportStatusCache.size());
        for (auto const& [id, status] : m_transportStatusCache) {
            snapshots.push_back(status);
        }
        return snapshots;
    }

    std::vector<transport::PeerSession>
    peerSessions(std::string const& id) const override {
        std::lock_guard lock(m_snapshotMutex);
        auto const it = m_peerSessionCache.find(id);
        return it == m_peerSessionCache.end()
            ? std::vector<transport::PeerSession>{} : it->second;
    }

    void setServerForwardEnabled(std::string const& serverTransportId, bool enabled) override {
        bool wasEnabled;
        {
            std::lock_guard lk(m_forwardMtx);
            auto it = m_forwardEnabled.find(serverTransportId);
            wasEnabled = (it != m_forwardEnabled.end()) ? it->second : true;
            m_forwardEnabled[serverTransportId] = enabled;
        }
        // 关闭瞬间(true→false):把该 server 桥接的转发区在程序内置 0,取消尚未写入
        // PLC 的操作箱指令(中性/停机)。只在边沿触发,避免每次调用重复 stage。
        if (wasEnabled && !enabled) {
            zeroBridgeForward(serverTransportId);
        }
    }

    bool serverForwardEnabled(std::string const& serverTransportId) const override {
        std::lock_guard lk(m_forwardMtx);
        auto it = m_forwardEnabled.find(serverTransportId);
        return (it != m_forwardEnabled.end()) ? it->second : true;
    }

    void start() override {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_started.load(std::memory_order_acquire)) {
            return;              // a second start() must not spawn a second
        }                        // concurrent connect() per transport
        m_started.store(true, std::memory_order_release);
        installEventWiring();
        // Connect transports in PARALLEL, off the calling (GUI) thread: a slow
        // or unreachable transport (e.g. a wrong MQTT / OPC UA endpoint) must
        // not stall start() for its whole connect timeout. The threads are
        // joined at stop()/teardown. Modules start polling immediately; reads
        // before a connection completes simply report "not connected".
        for (auto& [id, t] : m_transports) {
            transport::Transport* tp = t.get();
            m_connectThreads.emplace_back([tp]() { (void)tp->connect(); });
        }
        m_modules->startAll();
        startStatsPump();
        startMirrorPump();
        m_bus->publish(bus::CoreReady{});
        m_logger->logf(log::LogLevel::Info, "core", "ICore", "started");
    }

    void stop() override {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (!m_started.load(std::memory_order_acquire)) return;
        m_started.store(false, std::memory_order_release);
        m_logger->logf(log::LogLevel::Info, "core", "ICore", "stopping");
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
            auto const now = std::chrono::system_clock::now();
            std::optional<bus::SchedulerCircuitChanged> transition;
            {
                std::lock_guard lock(m_snapshotMutex);
                auto [it, inserted] =
                    m_schedulerCircuitCache.try_emplace(
                        id, stats.circuitState);
                if (!inserted && it->second != stats.circuitState) {
                    transition = bus::SchedulerCircuitChanged{
                        id, it->second, stats.circuitState, now};
                    it->second = stats.circuitState;
                }
            }
            if (transition) m_bus->publish(*transition);
            m_bus->publish(bus::SchedulerStatsEvent{id, stats});
        }
    }

    void setSchedulerStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }

private:
    static std::string
    summarizeErrors(config::ValidationErrors const& errors) {
        std::string summary;
        for (auto const& error : errors) {
            if (!summary.empty()) summary += "; ";
            summary += error.section + "." + error.field + ": "
                     + error.message;
        }
        return summary;
    }

    std::expected<void, config::ValidationErrors>
    buildRuntimeFromSchema(config::ConfigSchema const& schema,
                           std::string const& path) {
        m_configDir =
            QFileInfo(QString::fromStdString(path)).absolutePath();
        if (!schema.meta.logLevel.empty()) {
            m_logger->setThreshold(
                log::levelFromString(schema.meta.logLevel));
        }
        try {
            auto errors = wireFromSchema(schema);
            if (!errors.empty()) {
                return std::unexpected(std::move(errors));
            }
        } catch (std::exception const& error) {
            return std::unexpected(config::ValidationErrors{
                {"core", "initialization",
                 "runtime graph initialization failed: "
                     + std::string(error.what()),
                 -1}});
        } catch (...) {
            return std::unexpected(config::ValidationErrors{
                {"core", "initialization",
                 "runtime graph initialization failed with unknown exception",
                 -1}});
        }
        return {};
    }

    void destroyRuntimeGraph() {
        if (m_started.load(std::memory_order_acquire)) stop();
        stopStatsPump();
        stopMirrorPump();
        m_transportEventSub.reset();
        m_transportStateSub.reset();
        m_peerSessionSub.reset();
        m_pollRangeCompletedSub.reset();
        m_serverWriteSub.reset();
        joinConnectThreads();
        if (m_modules) m_modules->stopAll();

        // Transport destruction drains worker/scheduler callbacks. Keep
        // modules and bridge state alive until all transports are gone.
        m_transports.clear();
        m_pollRangePtrs.clear();
        m_sinkWindowPtrs.clear();
        m_bridgeFwdSinks.clear();
        m_modules.reset();

        // Plugins may own emitters bound into PortRegistry; unload their code
        // and objects before invalidating that registry.
        m_plugins.reset();
        m_ports.reset();
        m_datapointById.clear();
        m_routes.clear();
        m_bridgeMirrorStates.clear();
        m_bridges.clear();
        m_dps.reset();
        m_codecs.reset();
        {
            std::lock_guard lock(m_forwardMtx);
            m_forwardEnabled.clear();
        }
        {
            std::lock_guard lock(m_snapshotMutex);
            m_transportStatusCache.clear();
            m_peerSessionCache.clear();
            m_schedulerCircuitCache.clear();
        }
        m_configLoaded = false;
        m_activeSchema.reset();
        m_activeConfigPath.clear();
    }

    void initializeRuntimeGraph() {
        m_codecs = std::make_unique<codec::CodecRegistry>();
        m_codecs->loadBuiltins();
        m_dps = std::make_unique<dp::DatapointRegistry>();
        m_modules = std::make_unique<module::ModuleRegistry>();
        m_plugins = std::make_unique<plugin::PluginRegistry>();
    }

    config::ValidationErrors
    wireFromSchema(config::ConfigSchema const& schema) {
        auto errors = registerCustomCodecs(schema);
        if (!errors.empty()) return errors;
        buildTransports(schema);
        auto byId = buildDatapoints(schema);
        buildPollRanges(schema, byId);
        buildSinkWindows(schema);
        buildHeartbeats(schema);
        buildAckWatches(schema);
        buildCommands(schema);
        m_routes = schema.routes;
        buildBridges(schema);
        auto pluginErrors = loadPlugins(schema);
        errors.insert(errors.end(),
                      std::make_move_iterator(pluginErrors.begin()),
                      std::make_move_iterator(pluginErrors.end()));
        return errors;
    }

    // Load each [[plugin]] DLL, let it bind In/OutPorts to datapoints (which now
    // exist), then notify all that Core is wired. dll paths resolve against the
    // config dir when relative.
    config::ValidationErrors
    loadPlugins(config::ConfigSchema const& schema) {
        config::ValidationErrors errors;
        if (schema.plugins.empty()) return errors;
        m_ports = std::make_unique<plugin::PortRegistry>(*m_dps, *m_bus);
        for (std::size_t i = 0; i < schema.plugins.size(); ++i) {
            auto const& pc = schema.plugins[i];
            QString dll = qs(pc.dllPath);
            if (QFileInfo(dll).isRelative() && !m_configDir.isEmpty())
                dll = QDir(m_configDir).filePath(dll);
            if (!m_plugins->load(dll.toStdString())) {
                m_logger->logf(log::LogLevel::Error, "plugin", dll.toStdString(),
                               "failed to load plugin '" + pc.name + "'");
                errors.push_back(
                    {"plugin[" + std::to_string(i) + "]", "dll",
                     "failed to load plugin library '" + dll.toStdString()
                         + "' or resolve corePluginCreate",
                     -1});
            }
        }
        if (!errors.empty()) return errors;
        m_plugins->registerAllPorts(*m_ports);
        for (auto* p : m_plugins->all()) p->onInitialized();
        return errors;
    }

    void startStatsPump() {
        if (m_statsIntervalMs <= 0) return;
        if (m_statsTimer) return;
        m_statsTimer = new QTimer(&m_pump);
        m_statsTimer->setInterval(m_statsIntervalMs);
        m_statsTimer->setSingleShot(false);
        QObject::connect(m_statsTimer, &QTimer::timeout, &m_pump,
            [this]() { publishSchedulerStatsOnce(); });
        m_statsTimer->start();
    }

    void stopStatsPump() {
        if (!m_statsTimer) return;
        destroyTimer(m_statsTimer);
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
        m_transportStateSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::TransportStateChanged>(
                [this](bus::TransportStateChanged const& event) {
                    std::lock_guard lock(m_snapshotMutex);
                    m_transportStatusCache[event.after.transportId] =
                        event.after;
                }));
        m_peerSessionSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::PeerSessionChanged>(
                [this](bus::PeerSessionChanged const& event) {
                    std::lock_guard lock(m_snapshotMutex);
                    auto& sessions =
                        m_peerSessionCache[event.session.transportId];
                    if (event.kind
                        == bus::PeerSessionChangeKind::Connected) {
                        auto const exists = std::any_of(
                            sessions.cbegin(), sessions.cend(),
                            [&](transport::PeerSession const& session) {
                                return session.sessionId
                                    == event.session.sessionId;
                            });
                        if (!exists) sessions.push_back(event.session);
                    } else {
                        std::erase_if(
                            sessions,
                            [&](transport::PeerSession const& session) {
                                return session.sessionId
                                    == event.session.sessionId;
                            });
                    }
                }));
        m_pollRangeCompletedSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::PollRangeCompleted>(
                [this](bus::PollRangeCompleted const& e) {
                    onPollRangeCompleted(e);
                }));
        // On operator-box write into a server transport, fan out into a
        // SinkWindow on the PLC-side transport via the configured routes.
        m_serverWriteSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::ServerWriteEvent>(
                [this](bus::ServerWriteEvent const& e) {
                    log::OperationRecord op;
                    op.actor    = "operator-box";
                    op.action   = "server-write";
                    op.target   = e.transportId;
                    op.newValue = std::to_string(e.values.size()) + " regs @ "
                                + std::to_string(e.startAddress);
                    op.result   = "ok";
                    op.category = "audit";
                    op.eventKey = "server-write:" + std::to_string(e.startAddress);
                    m_logger->logOperation(std::move(op));
                    if (!serverForwardEnabled(e.transportId)) return;   // 业务闸门:不转发
                    routeServerWrite(e);
                    forwardBridges(e);
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
        m_logger->logf(level, "transport", e.transportId, what);
    }

    void refreshSnapshotCache() {
        std::map<std::string, transport::TransportStatus> statuses;
        std::map<std::string, std::vector<transport::PeerSession>> peers;
        for (auto const& [id, live] : m_transports) {
            auto status = live->status();
            status.transportId = id;
            if (status.changedAt.time_since_epoch().count() == 0) {
                status.changedAt = std::chrono::system_clock::now();
            }
            statuses.emplace(id, std::move(status));
            peers.emplace(id, live->peerSessions());
        }
        std::lock_guard lock(m_snapshotMutex);
        m_transportStatusCache = std::move(statuses);
        m_peerSessionCache = std::move(peers);
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
                if (!sink.window.empty() && sw->id() == sink.window) {
                    target = sw;
                    break;
                }
            }
            if (!target && !sink.transport.empty()) {
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

    // ——— 整段桥接(替代旧 ModbusServer 中继) ———
    void buildBridges(config::ConfigSchema const& schema) {
        m_bridges = schema.bridges;
        m_bridgeMirrorStates.clear();
        m_bridgeMirrorStates.reserve(m_bridges.size());
        for (size_t i = 0; i < m_bridges.size(); ++i) {
            m_bridgeMirrorStates.push_back(
                std::make_shared<BridgeMirrorState>());
        }
        m_bridgeFwdSinks.assign(m_bridges.size(), nullptr);
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            // 转发走 PLC 侧的 SinkWindow(同 route 路径):线程安全地 stageRegister,
            // 由 TickDriver 在生命周期线程带重试/coalesce 地刷到 PLC —— 避免单次
            // submitAsync 在调度器繁忙时被丢弃。
            if (b.writeCount > 0) {
                if (auto* plc = transport(b.plc)) {
                    module::SinkWindow::Config cfg;
                    cfg.moduleId       = "bridge.fwd." + b.server;
                    cfg.table          = core::RegisterTable::HoldingRegister;
                    cfg.startAddress   = b.writeStart - b.offset;
                    cfg.size           = b.writeCount;
                    cfg.priority       = sched::Priority::High;
                    cfg.debounceMs     = 0;
                    cfg.keepAlivePeriodMs = 0;
                    cfg.coalesceWrites = true;
                    auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *plc);
                    m_bridgeFwdSinks[size_t(i)] = sw.get();
                    m_sinkWindowPtrs.push_back(sw.get());
                    m_modules->registerModule(std::move(sw));
                }
            }
        }
    }

    // 操作箱写 server → 写区子段 stage 到 PLC 侧 SinkWindow(server 地址 - offset)。
    // stageRegister 线程安全,可直接在 server transport 线程调用;刷写由 TickDriver 做。
    void forwardBridges(bus::ServerWriteEvent const& e) {
        if (e.table != core::RegisterTable::HoldingRegister) return;
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
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
    void zeroBridgeForward(std::string const& server) {
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (b.server != server) continue;
            auto* sink = m_bridgeFwdSinks[size_t(i)];
            if (!sink) continue;
            for (int a = b.writeStart; a < b.writeStart + b.writeCount; ++a) {
                sink->stageRegister(a - b.offset, 0);
            }
            // Disabling is a safety edge: the PLC may still contain a command
            // written by another master or a prior process instance.
            sink->forceFlush();
        }
    }

public:
    // Test hook and timer entry: only Periodic bridge policies run here.
    void mirrorBridgesOnce() {
        mirrorBridgesPeriodically();
    }

private:
    void onPollRangeCompleted(bus::PollRangeCompleted const& e) {
        if (e.table != core::RegisterTable::HoldingRegister) return;
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (b.mirrorCount <= 0 || b.plc != e.transportId) continue;
            int const offset = b.mirrorStart - e.startAddress;
            if (offset < 0
                || offset + b.mirrorCount > int(e.values.size())) {
                continue;
            }
            auto const state = m_bridgeMirrorStates[size_t(i)];
            {
                std::lock_guard lock(state->mutex);
                state->values.assign(e.values.begin() + offset,
                                     e.values.begin() + offset + b.mirrorCount);
                ++state->version;
            }
            if (b.mirrorPolicy == config::BridgeMirrorPolicy::AfterPoll) {
                scheduleBridgeMirror(i);
            }
        }
    }

    void scheduleBridgeMirror(int i) {
        if (i < 0 || i >= int(m_bridges.size())) return;
        auto const& b = m_bridges[size_t(i)];
        if (b.mirrorCount <= 0) return;
        auto* server = transport(b.server);
        if (!server) return;
        auto const state = m_bridgeMirrorStates[size_t(i)];
        core::RegisterWords values;
        std::uint64_t version = 0;
        {
            std::lock_guard lock(state->mutex);
            if (state->inFlight
                || int(state->values.size()) != b.mirrorCount) {
                return;
            }
            state->inFlight = true;
            values = state->values;
            version = state->version;
        }

        transport::WriteBatch batch;
        batch.table = core::RegisterTable::HoldingRegister;
        batch.startAddress = b.mirrorStart + b.offset;
        batch.values = std::move(values);
        sched::RequestTag tag;
        tag.moduleId = "bridge.mirror." + b.server + "." + std::to_string(i);
        tag.priority = sched::Priority::Low;

        auto const submitted = server->scheduler().submitAsync(
            tag,
            [this, i, server, batch = std::move(batch), state, version](
                sched::AsyncDone done) mutable {
                try {
                    server->writeAsync(
                        batch,
                        [this, i, state, version,
                         done = std::move(done)](
                            transport::WriteResult result) mutable {
                            bool repeat = false;
                            {
                                std::lock_guard lock(state->mutex);
                                state->inFlight = false;
                                repeat = state->version > version;
                            }
                            done(result.ok);
                            if (repeat
                                && m_started.load(std::memory_order_acquire)
                                && i < int(m_bridges.size())
                                && m_bridges[size_t(i)].mirrorPolicy
                                    == config::BridgeMirrorPolicy::AfterPoll) {
                                scheduleBridgeMirror(i);
                            }
                        });
                } catch (...) {
                    std::lock_guard lock(state->mutex);
                    state->inFlight = false;
                    throw;
                }
            });
        if (submitted.kind == sched::ResultKind::Ok) {
            std::lock_guard lock(state->mutex);
            state->lastSubmittedAt = std::chrono::steady_clock::now();
        } else {
            std::lock_guard lock(state->mutex);
            state->inFlight = false;
        }
    }

    void mirrorBridgesPeriodically() {
        auto const now = std::chrono::steady_clock::now();
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (b.mirrorCount <= 0
                || b.mirrorPolicy != config::BridgeMirrorPolicy::Periodic) {
                continue;
            }
            auto const state = m_bridgeMirrorStates[size_t(i)];
            {
                std::lock_guard lock(state->mutex);
                if (state->lastSubmittedAt.time_since_epoch().count() > 0
                    && now - state->lastSubmittedAt
                        < std::chrono::milliseconds(b.mirrorPeriodMs)) {
                    continue;
                }
            }
            scheduleBridgeMirror(i);
        }
    }

    void startMirrorPump() {
        if (m_mirrorTimer) return;
        int period = std::numeric_limits<int>::max();
        bool any = false;
        for (auto const& b : m_bridges) {
            if (b.mirrorCount > 0
                && b.mirrorPolicy == config::BridgeMirrorPolicy::Periodic) {
                any = true;
                period = std::min(period, std::max(20, b.mirrorPeriodMs));
            }
        }
        if (!any) return;
        m_mirrorTimer = new QTimer(&m_pump);
        m_mirrorTimer->setInterval(period);
        m_mirrorTimer->setSingleShot(false);
        QObject::connect(m_mirrorTimer, &QTimer::timeout, &m_pump,
            [this]() { mirrorBridgesOnce(); });
        m_mirrorTimer->start();
    }

    void stopMirrorPump() {
        if (!m_mirrorTimer) return;
        destroyTimer(m_mirrorTimer);
    }

    static void destroyTimer(QTimer*& slot) {
        auto* timer = std::exchange(slot, nullptr);
        if (!timer) return;
        if (QThread::currentThread() == timer->thread()) {
            timer->stop();
            delete timer;
            return;
        }
        QMetaObject::invokeMethod(
            timer,
            [timer] {
                timer->stop();
                delete timer;
            },
            Qt::BlockingQueuedConnection);
    }

    config::ValidationErrors
    registerCustomCodecs(config::ConfigSchema const& schema) {
        config::ValidationErrors errors;
        for (std::size_t i = 0; i < schema.codecs.size(); ++i) {
            auto const& cc = schema.codecs[i];
            if (cc.kind == "enum_u16") {
                std::unordered_map<std::uint16_t, std::string> map;
                for (auto const& [k, v] : cc.map) {
                    try {
                        auto const parsed = std::stoul(k);
                        if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                            throw std::out_of_range("enum key exceeds u16");
                        }
                        auto raw = static_cast<std::uint16_t>(parsed);
                        map.emplace(raw, dp::toString(v));
                    } catch (...) {
                        errors.push_back(
                            {"codec[" + std::to_string(i) + "]", "map." + k,
                             "enum_u16 keys must be decimal integers in "
                             "the range 0..65535",
                             -1});
                    }
                }
                if (errors.empty()) {
                    m_codecs->registerCodec(
                        std::make_shared<codec::EnumU16Codec>(
                            cc.id, std::move(map)));
                }
            } else if (cc.kind == "lua") {
                QString script = qs(cc.script);
                if (QFileInfo(script).isRelative() && !m_configDir.isEmpty())
                    script = QDir(m_configDir).filePath(script);
                std::string err;
                auto lc = codec::LuaCodec::fromFile(cc.id, script.toStdString(), cc.arg, &err);
                if (lc) {
                    m_codecs->registerCodec(std::move(lc));
                } else {
                    auto const message =
                        err.empty()
                        ? std::string("lua codec load failed")
                        : err;
                    m_logger->logf(log::LogLevel::Error, "config",
                                   script.toStdString(),
                                   message);
                    errors.push_back(
                        {"codec[" + std::to_string(i) + "]", "script",
                         "failed to load Lua codec '" + script.toStdString()
                             + "': " + message,
                         -1});
                }
            }
        }
        return errors;
    }

    void buildTransports(config::ConfigSchema const& schema) {
        for (auto const& tc : schema.transports) {
            if (tc.kind == transport::TransportKind::ModbusTcpClient) {
                transport::ModbusTcpClientTransport::Config cfg;
                cfg.id                   = tc.id;
                cfg.host                 = tc.host;
                cfg.port                 = std::uint16_t(tc.port);
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
                cfg.listenPort           = std::uint16_t(tc.listenPort);
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

    using DpById = std::map<std::string, std::shared_ptr<dp::Datapoint>>;

    void buildSinkWindows(config::ConfigSchema const& schema) {
        for (auto const& sc : schema.sinkWindows) {
            auto* t = transport(sc.transport);
            if (!t) continue;
            module::SinkWindow::Config cfg;
            cfg.moduleId          = sc.moduleId;
            cfg.table             = tableFromString(qs(sc.table));
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
            cfg.table    = tableFromString(qs(hc.table));
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
                e.table   = tableFromString(qs(w.table));
                e.address = w.address;
                e.value   = w.value;
                cfg.writes.push_back(e);
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
                if (!dc.source.codec.empty()) {
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
            if (dc.hasSource && dc.onDisconnect != "hold") {
                datapoint->setDisconnectValue(
                    makeDisconnectValue(dc.type, dc.disconnectValue));
            }
            m_dps->registerDp(datapoint);
            out.emplace(dc.id, datapoint);
            m_datapointById.emplace(dc.id, datapoint);

            // Auto-publish DpChanged on every value change so plugins /
            // database / dashboard subscribers don't need to poll. The model is
            // Qt-free now, so we hop to the bus (QObject) thread ourselves to
            // preserve the old queued-signal semantics: setValue runs on a
            // transport worker thread, DpChanged is published on the bus thread.
            std::weak_ptr<dp::Datapoint> weak = datapoint;
            datapoint->setOnValueChanged([this, weak] {
                QMetaObject::invokeMethod(&m_pump, [this, weak] {
                    auto sp = weak.lock();
                    if (!sp) return;
                    auto const snap = sp->snapshot();
                    m_bus->publish(bus::DpChanged{
                        sp->id(), snap.value, snap.timestamp});
                }, Qt::QueuedConnection);
            });

            // For Command / Bidirectional datapoints with a SinkWindow sink,
            // wire a writer that encodes the value through the codec then
            // stages into the named SinkWindow. QML calls `dp.write(v)` and
            // the new value lands on the PLC at the next flush tick.
            if ((datapoint->kind() == dp::Kind::Command
                || datapoint->kind() == dp::Kind::Bidirectional)
                && datapoint->sink().has_value()
                && !datapoint->sink()->window.empty()) {
                std::string const windowId = datapoint->sink()->window;
                datapoint->setWriter([this, weak, windowId](core::dp::Value const& v) {
                    auto sp = weak.lock();
                    if (!sp || !sp->sink().has_value()) return;
                    auto sink = *sp->sink();
                    if (!sink.codec) return;
                    auto encoded = sink.codec->encode(v, sink);
                    if (encoded.empty()) return;
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
            req.table        = tableFromString(qs(pc.table));
            req.startAddress = pc.startAddress;
            req.count        = pc.count;

            auto poll = std::make_unique<module::PollRange>(
                pc.moduleId, *t, req, pc.periodMs, pc.priority, m_bus.get());
            wireBindings(*poll, schema, byId, pc.transport, req);

            auto* raw = poll.get();
            m_modules->registerModule(std::move(poll));
            m_pollRangePtrs.push_back(raw);
        }
    }

    void wireBindings(module::PollRange&            poll,
                       config::ConfigSchema const&    schema,
                       DpById const&                   byId,
                       std::string const&              transportId,
                       transport::ReadRequest const&   req) {
        for (auto const& dc : schema.datapoints) {
            if (!dc.hasSource) continue;
            if (dc.source.port != transportId) continue;
            if (tableFromString(qs(dc.source.table)) != req.table) continue;
            int const rc     = dp::registerCountFor(dc.type);
            int const offset = dc.source.address - req.startAddress;
            if (offset < 0 || offset + rc > req.count) continue;
            auto itDp = byId.find(dc.id);
            if (itDp == byId.end()) continue;

            auto codec = m_codecs->find(dc.source.codec.empty()
                ? codec::BuiltinScalarCodec::idFor(dc.type)
                : dc.source.codec);
            if (!codec) continue;
            poll.bind(itDp->second, codec, offset);
        }
    }

private:
    struct BridgeMirrorState {
        std::mutex                            mutex;
        core::RegisterWords                   values;
        std::uint64_t                         version = 0;
        bool                                  inFlight = false;
        std::chrono::steady_clock::time_point lastSubmittedAt;
    };

    // Qt-thread anchor for the (now Qt-free) EventBus: owns the stats/mirror
    // QTimers and is the target for queued DpChanged publication, so value
    // changes pushed on a transport worker thread are published on this (GUI)
    // thread — the semantics the old QObject EventBus provided.
    QObject                                                     m_pump;
    QString                                                     m_configDir;
    std::unique_ptr<log::Logger>                                m_logger;
#ifdef CORE_HAS_QML
    std::unique_ptr<qml::LogBridge>                             m_logBridge;
#endif
    std::unique_ptr<bus::EventBus>                              m_bus;
    std::unique_ptr<codec::CodecRegistry>                       m_codecs;
    std::unique_ptr<dp::DatapointRegistry>                      m_dps;
    std::unique_ptr<module::ModuleRegistry>                     m_modules;
    std::unique_ptr<plugin::PluginRegistry>                     m_plugins;
    std::unique_ptr<plugin::PortRegistry>                       m_ports;
    std::map<std::string, std::unique_ptr<transport::Transport>> m_transports;
    std::vector<std::thread>                                    m_connectThreads;
    mutable std::recursive_mutex                                m_lifecycleMutex;
    std::atomic_bool                                            m_started{false};
    std::atomic_bool                                            m_reloadInProgress{false};
    bool                                                        m_configLoaded = false;
    std::optional<config::ConfigSchema>                          m_activeSchema;
    std::string                                                 m_activeConfigPath;
    std::uint64_t                                               m_datapointGeneration = 0;
    std::vector<module::PollRange*>                             m_pollRangePtrs;
    std::vector<module::SinkWindow*>                            m_sinkWindowPtrs;
    std::map<std::string, std::shared_ptr<dp::Datapoint>>      m_datapointById;
    std::vector<config::RouteConfig>                            m_routes;
    std::vector<config::BridgeConfig>                           m_bridges;
    std::vector<std::shared_ptr<BridgeMirrorState>>              m_bridgeMirrorStates;
    std::vector<module::SinkWindow*>                            m_bridgeFwdSinks;
    mutable std::mutex                                          m_forwardMtx;
    std::unordered_map<std::string, bool>                       m_forwardEnabled;   // server id → 转发使能(默认 true)
    mutable std::mutex                                          m_snapshotMutex;
    std::map<std::string, transport::TransportStatus>           m_transportStatusCache;
    std::map<std::string, std::vector<transport::PeerSession>>  m_peerSessionCache;
    std::map<std::string, sched::CircuitState>                   m_schedulerCircuitCache;
    QTimer*                                                     m_mirrorTimer = nullptr;
    std::unique_ptr<bus::Subscription>                          m_transportEventSub;
    std::unique_ptr<bus::Subscription>                          m_transportStateSub;
    std::unique_ptr<bus::Subscription>                          m_peerSessionSub;
    std::unique_ptr<bus::Subscription>                          m_pollRangeCompletedSub;
    std::unique_ptr<bus::Subscription>                          m_serverWriteSub;
    QTimer*                                                     m_statsTimer = nullptr;
    int                                                         m_statsIntervalMs = 1000;
};

std::unique_ptr<ICore> ICore::create(bool installDefaultConsole) {
    return std::make_unique<CoreImpl>(installDefaultConsole);
}

#ifdef CORE_HAS_QML
namespace qml {
std::unique_ptr<ICore> createWithQml(QQmlContext* ctx, bool installDefaultConsole) {
    auto impl = std::make_unique<CoreImpl>(installDefaultConsole);
    impl->wireQml(ctx);
    return impl;
}
} // namespace qml
#endif

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
