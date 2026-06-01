#include "core/ICore.h"
#include "core/internal/Testing.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QHash>
#include <QQmlContext>
#include <QtSerialBus/QModbusDataUnit>

#include "core/bus/EventBus.h"
#include "core/bus/BusEvents.h"
#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/module/ModuleRegistry.h"
#include "core/module/PollRange.h"
#include "core/plugin/PluginRegistry.h"
#include "core/transport/ModbusTcpClientTransport.h"
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
    return p;
}

} // namespace

class CoreImpl : public ICore {
public:
    explicit CoreImpl(QQmlContext* qml)
        : m_qml(qml)
        , m_bus(std::make_unique<bus::EventBus>())
        , m_codecs(std::make_unique<codec::CodecRegistry>())
        , m_dps(std::make_unique<dp::DatapointRegistry>())
        , m_modules(std::make_unique<module::ModuleRegistry>())
        , m_plugins(std::make_unique<plugin::PluginRegistry>()) {
        m_codecs->loadBuiltins();
    }

    ~CoreImpl() override {
        // Deterministic teardown order — modules first (they hold raw
        // Transport*), then transports (each one stops its worker thread),
        // then datapoints / codecs / bus. Letting the default destructor
        // run member-by-member in reverse declaration order would tear
        // down transports while PollRange* are still alive.
        m_modules.reset();
        m_pollRangePtrs.clear();
        m_transports.clear();
        m_plugins.reset();
        m_dps.reset();
        m_codecs.reset();
        m_bus.reset();
    }

    std::expected<void, config::ValidationErrors>
    loadConfig(QString const& path) override {
        config::ConfigLoader loader;
        auto schema = loader.loadFromToml(path);
        if (!schema.has_value()) return std::unexpected(schema.error());

        wireFromSchema(*schema);
        return {};
    }

    bus::EventBus&            bus()        override { return *m_bus; }
    dp::DatapointRegistry&    datapoints() override { return *m_dps; }
    codec::CodecRegistry&     codecs()     override { return *m_codecs; }
    module::ModuleRegistry&   modules()    override { return *m_modules; }
    plugin::PluginRegistry&   plugins()    override { return *m_plugins; }

    transport::Transport* transport(QString const& id) const override {
        auto it = m_transports.find(id);
        return it == m_transports.end() ? nullptr : it->second.get();
    }

    void start() override {
        for (auto& [id, t] : m_transports) {
            (void)t->connect();
        }
        m_modules->startAll();
        m_bus->publish(bus::CoreReady{});
    }

    void stop() override {
        m_bus->publish(bus::CoreStopping{});
        m_modules->stopAll();
        for (auto& [id, t] : m_transports) t->disconnect();
    }

    void pollAllOnce() {
        for (auto* poll : m_pollRangePtrs) poll->pollOnce();
    }

private:
    void wireFromSchema(config::ConfigSchema const& schema) {
        registerCustomCodecs(schema);
        buildTransports(schema);
        auto byId = buildDatapoints(schema);
        buildPollRanges(schema, byId);
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
            if (tc.kind != transport::TransportKind::ModbusTcpClient) continue;
            transport::ModbusTcpClientTransport::Config cfg;
            cfg.id               = tc.id;
            cfg.host             = tc.host;
            cfg.port             = tc.port;
            cfg.slaveId          = tc.slaveId;
            cfg.connectTimeoutMs = tc.connectTimeoutMs;
            cfg.scheduler        = tc.scheduler;
            m_transports.emplace(
                tc.id,
                std::make_unique<transport::ModbusTcpClientTransport>(std::move(cfg)));
        }
    }

    using DpById = std::map<QString, std::shared_ptr<dp::Datapoint>>;

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
    std::unique_ptr<bus::EventBus>                              m_bus;
    std::unique_ptr<codec::CodecRegistry>                       m_codecs;
    std::unique_ptr<dp::DatapointRegistry>                      m_dps;
    std::unique_ptr<module::ModuleRegistry>                     m_modules;
    std::unique_ptr<plugin::PluginRegistry>                     m_plugins;
    std::map<QString, std::unique_ptr<transport::Transport>>    m_transports;
    std::vector<module::PollRange*>                             m_pollRangePtrs;
};

std::unique_ptr<ICore> ICore::create(QQmlContext* qml) {
    return std::make_unique<CoreImpl>(qml);
}

namespace internal {
void pollAllOnce(ICore& core) {
    static_cast<CoreImpl&>(core).pollAllOnce();
}
} // namespace internal

} // namespace core
