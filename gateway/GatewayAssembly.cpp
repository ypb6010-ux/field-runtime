// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "GatewayAssembly.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <utility>

#include "AsioModbusTcpClient.h"
#include "AsioModbusTcpServer.h"
#include "StubTransport.h"

#include "core/bus/BusEvents.h"
#include "core/codec/BuiltinCodecs.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/ScalarType.h"
#include "core/dp/Value.h"
#include "core/dp/WordOrder.h"
#include "core/log/Sinks.h"
#include "core/module/SinkWindow.h"

namespace core::gateway {

namespace {

core::RegisterTable tableFromString(std::string const& s) {
    if (s == "IR" || s == "InputRegisters") return core::RegisterTable::InputRegister;
    if (s == "Coil" || s == "Coils") return core::RegisterTable::Coil;
    if (s == "DI" || s == "DiscreteInputs") return core::RegisterTable::DiscreteInput;
    return core::RegisterTable::HoldingRegister;
}

dp::WordOrder wordOrderFromString(std::string const& s) {
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    return dp::WordOrder::ABCD;
}

dp::Kind kindFromString(std::string const& s) {
    if (s == "Command") return dp::Kind::Command;
    if (s == "Bidirectional") return dp::Kind::Bidirectional;
    return dp::Kind::Status;
}

dp::PortRef makePortRef(config::PortRefConfig const& pc,
                        std::shared_ptr<codec::Codec> codec) {
    dp::PortRef p;
    p.transport = pc.port;
    p.table = tableFromString(pc.table);
    p.address = pc.address;
    if (pc.bit >= 0) p.bit = pc.bit;
    p.wordOrder = wordOrderFromString(pc.wordOrder);
    p.shift = pc.shift;
    p.mask = pc.mask;
    p.scale = pc.scale;
    p.offset = pc.offset;
    p.codec = std::move(codec);
    p.window = pc.window;
    return p;
}

std::string valueText(dp::Value const& value) {
    return dp::toString(value);
}

} // namespace

GatewayAssembly::GatewayAssembly(gateway_asio::io_context& io)
    : m_io(&io) {
    m_codecs.loadBuiltins();
    m_logger.addSink(std::make_shared<log::ConsoleSink>());
    m_logger.addSink(std::make_shared<log::RollingFileSink>("field_gateway.log"));
}

GatewayAssembly::~GatewayAssembly() {
    stop();
    m_logger.stop();
}

log::Logger& GatewayAssembly::logger() {
    return m_logger;
}

bool GatewayAssembly::load(std::string const& tomlPath) {
    config::ConfigLoader loader;
    auto loaded = loader.loadFromToml(tomlPath);
    if (!loaded.has_value()) {
        for (auto const& err : loaded.error()) {
            std::cerr << err.section << "." << err.field << ": "
                      << err.message << '\n';
            m_logger.logf(log::LogLevel::Error, "config", tomlPath, err.message);
        }
        return false;
    }

    if (!loaded->meta.logLevel.empty()) {
        m_logger.setThreshold(log::levelFromString(loaded->meta.logLevel));
    }

    wireFromSchema(*loaded);
    m_logger.logf(log::LogLevel::Info, "gateway", "assembly", "loaded",
                  {{"transports", std::int64_t(loaded->transports.size())},
                   {"datapoints", std::int64_t(loaded->datapoints.size())},
                   {"poll_ranges", std::int64_t(loaded->pollRanges.size())}});
    return true;
}

void GatewayAssembly::start() {
    if (m_started) return;
    m_started = true;
    installEventWiring();

    for (auto& [id, transport] : m_transports) {
        auto connected = transport->connect();
        if (connected.has_value()) {
            m_bus.publish(bus::TransportEvent{id, bus::TransportEventKind::Connected, {}});
            m_logger.logf(log::LogLevel::Info, "transport", id, "connected");
        } else {
            m_logger.logf(log::LogLevel::Error, "transport", id, connected.error());
        }
    }

    for (auto& poll : m_pollRanges) poll->start();
    for (auto& sink : m_sinkWindows) sink->start();
    for (auto& timer : m_pollTimers) schedulePoll(timer);
    for (auto& timer : m_sinkTimers) scheduleSink(timer);
    startMirrorPump();
    m_bus.publish(bus::CoreReady{});
}

void GatewayAssembly::stop() {
    if (!m_started) return;
    m_started = false;

    for (auto& timer : m_pollTimers) {
        if (timer.timer) timer.timer->cancel();
    }
    for (auto& timer : m_sinkTimers) {
        if (timer.timer) timer.timer->cancel();
    }
    if (m_mirrorTimer) m_mirrorTimer->cancel();
    for (auto& poll : m_pollRanges) poll->stop();
    for (auto& sink : m_sinkWindows) sink->stop();
    for (auto& [id, transport] : m_transports) {
        transport->disconnect();
        m_bus.publish(bus::TransportEvent{id, bus::TransportEventKind::Disconnected, {}});
    }
    m_serverWriteSub.reset();
    m_bus.publish(bus::CoreStopping{});
    m_logger.flush();
}

void GatewayAssembly::setServerForwardEnabled(std::string const& serverTransportId,
                                              bool enabled) {
    bool wasEnabled = true;
    {
        std::lock_guard lk(m_forwardMtx);
        auto it = m_forwardEnabled.find(serverTransportId);
        if (it != m_forwardEnabled.end()) wasEnabled = it->second;
        m_forwardEnabled[serverTransportId] = enabled;
    }
    if (wasEnabled && !enabled) zeroBridgeForward(serverTransportId);
}

void GatewayAssembly::printSnapshot(std::size_t limit) const {
    auto all = m_datapoints.all();
    std::sort(all.begin(), all.end(), [](auto const& a, auto const& b) {
        return a->id() < b->id();
    });

    std::cout << "snapshot";
    std::size_t n = 0;
    for (auto const& dp : all) {
        if (n++ >= limit) break;
        auto const state = dp->snapshot();
        std::cout << ' ' << dp->id() << '=' << valueText(state.value)
                  << '(' << dp->stateText() << ')';
    }
    std::cout << std::endl;
}

void GatewayAssembly::wireFromSchema(config::ConfigSchema const& schema) {
    buildTransports(schema);
    auto byId = buildDatapoints(schema);
    buildPollRanges(schema, byId);
    buildSinkWindows(schema);
    buildBridges(schema);
}

void GatewayAssembly::buildTransports(config::ConfigSchema const& schema) {
    for (auto const& tc : schema.transports) {
        std::unique_ptr<transport::Transport> transport;
        if (tc.kind == transport::TransportKind::ModbusTcpClient) {
            transport = std::make_unique<AsioModbusTcpClient>(tc, *m_io);
        } else if (tc.kind == transport::TransportKind::ModbusTcpServer) {
            transport = std::make_unique<AsioModbusTcpServer>(tc, *m_io, m_bus);
        } else {
            transport = std::make_unique<StubTransport>(tc, *m_io);
        }
        m_transports.emplace(tc.id, std::move(transport));
    }
}

GatewayAssembly::DpById GatewayAssembly::buildDatapoints(config::ConfigSchema const& schema) {
    DpById out;
    for (auto const& dc : schema.datapoints) {
        std::shared_ptr<codec::Codec> sourceCodec;
        if (dc.hasSource) {
            if (!dc.source.codec.empty()) {
                sourceCodec = m_codecs.find(dc.source.codec);
            }
            if (!sourceCodec) {
                sourceCodec = m_codecs.find(codec::BuiltinScalarCodec::idFor(dc.type));
            }
        }

        dp::DatapointSpec spec;
        spec.id = dc.id;
        spec.kind = kindFromString(dc.kind);
        spec.type = dc.type;
        if (dc.hasSource) spec.source = makePortRef(dc.source, sourceCodec);
        if (dc.hasSink) {
            spec.sink = makePortRef(
                dc.sink,
                m_codecs.find(codec::BuiltinScalarCodec::idFor(dc.type)));
        }
        spec.uiBinding = dc.ui;
        spec.persistTag = dc.persist;

        auto datapoint = std::make_shared<dp::Datapoint>(std::move(spec));
        std::weak_ptr<dp::Datapoint> weak = datapoint;
        datapoint->setOnValueChanged([this, weak] {
            auto sp = weak.lock();
            if (!sp) return;
            auto const snap = sp->snapshot();
            m_bus.publish(bus::DpChanged{sp->id(), snap.value, snap.timestamp});
        });

        m_datapoints.registerDp(datapoint);
        out.emplace(dc.id, std::move(datapoint));
    }
    return out;
}

void GatewayAssembly::buildPollRanges(config::ConfigSchema const& schema,
                                      DpById const& byId) {
    for (auto const& pc : schema.pollRanges) {
        auto it = m_transports.find(pc.transport);
        if (it == m_transports.end()) continue;

        transport::ReadRequest req;
        req.table = tableFromString(pc.table);
        req.startAddress = pc.startAddress;
        req.count = pc.count;

        auto poll = std::make_unique<module::PollRange>(
            pc.moduleId, *it->second, req, pc.periodMs, pc.priority);
        wireBindings(*poll, schema, byId, pc.transport, req);

        m_pollTimers.push_back(PollTimer{
            poll.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_pollRanges.push_back(std::move(poll));
    }
}

void GatewayAssembly::buildSinkWindows(config::ConfigSchema const& schema) {
    for (auto const& sc : schema.sinkWindows) {
        auto it = m_transports.find(sc.transport);
        if (it == m_transports.end()) continue;

        module::SinkWindow::Config cfg;
        cfg.moduleId = sc.moduleId;
        cfg.table = tableFromString(sc.table);
        cfg.startAddress = sc.startAddress;
        cfg.size = sc.size;
        cfg.priority = sc.priority;
        cfg.debounceMs = sc.flush.debounceMs;
        cfg.keepAlivePeriodMs = sc.flush.keepaliveMs;
        cfg.coalesceWrites = sc.flush.coalesceWrites;
        cfg.initial = sc.initial;

        auto sink = std::make_unique<module::SinkWindow>(std::move(cfg), *it->second);
        m_sinkTimers.push_back(SinkTimer{
            sink.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_sinkWindows.push_back(std::move(sink));
    }
}

void GatewayAssembly::buildBridges(config::ConfigSchema const& schema) {
    m_bridges = schema.bridges;
    m_bridgeMirrors.assign(m_bridges.size(), {});
    m_bridgeFwdSinks.assign(m_bridges.size(), nullptr);

    auto allDps = m_datapoints.all();
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        auto& mirrors = m_bridgeMirrors[size_t(i)];
        for (auto const& dp : allDps) {
            auto const& src = dp->source();
            if (!src.has_value()) continue;
            if (src->transport != bridge.plc) continue;
            if (src->table != core::RegisterTable::HoldingRegister) continue;
            int const address = src->address;
            if (address < bridge.mirrorStart
                || address >= bridge.mirrorStart + bridge.mirrorCount) {
                continue;
            }
            mirrors.emplace_back(address, dp);
        }

        if (bridge.writeCount <= 0) continue;
        auto it = m_transports.find(bridge.plc);
        if (it == m_transports.end()) continue;

        module::SinkWindow::Config cfg;
        cfg.moduleId = "bridge.fwd." + bridge.server;
        cfg.table = core::RegisterTable::HoldingRegister;
        cfg.startAddress = bridge.writeStart - bridge.offset;
        cfg.size = bridge.writeCount;
        cfg.priority = sched::Priority::High;
        cfg.debounceMs = 0;
        cfg.keepAlivePeriodMs = 0;
        cfg.coalesceWrites = true;

        auto sink = std::make_unique<module::SinkWindow>(std::move(cfg), *it->second);
        m_bridgeFwdSinks[size_t(i)] = sink.get();
        m_sinkTimers.push_back(SinkTimer{
            sink.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_sinkWindows.push_back(std::move(sink));
    }
}

void GatewayAssembly::installEventWiring() {
    if (m_serverWriteSub) return;
    m_serverWriteSub = std::make_unique<bus::Subscription>(
        m_bus.subscribe<bus::ServerWriteEvent>(
            [this](bus::ServerWriteEvent const& e) {
                m_logger.logf(log::LogLevel::Info,
                              "gateway",
                              e.transportId,
                              "server-write "
                                  + std::to_string(e.values.size())
                                  + " regs @ "
                                  + std::to_string(e.startAddress));
                if (!serverForwardEnabled(e.transportId)) return;
                forwardBridges(e);
            }));
}

bool GatewayAssembly::serverForwardEnabled(std::string const& serverTransportId) const {
    std::lock_guard lk(m_forwardMtx);
    auto it = m_forwardEnabled.find(serverTransportId);
    return it == m_forwardEnabled.end() ? true : it->second;
}

void GatewayAssembly::forwardBridges(bus::ServerWriteEvent const& e) {
    if (e.table != core::RegisterTable::HoldingRegister) return;
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.server != e.transportId) continue;
        auto* sink = m_bridgeFwdSinks[size_t(i)];
        if (!sink) continue;

        int const eventStart = e.startAddress;
        int const eventEnd = e.startAddress + int(e.values.size());
        int const start = std::max(eventStart, bridge.writeStart);
        int const end = std::min(eventEnd, bridge.writeStart + bridge.writeCount);
        for (int address = start; address < end; address++) {
            sink->stageRegister(address - bridge.offset, e.values[size_t(address - eventStart)]);
        }
    }
}

void GatewayAssembly::zeroBridgeForward(std::string const& serverTransportId) {
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.server != serverTransportId) continue;
        auto* sink = m_bridgeFwdSinks[size_t(i)];
        if (!sink) continue;
        for (int address = bridge.writeStart;
             address < bridge.writeStart + bridge.writeCount;
             address++) {
            sink->stageRegister(address - bridge.offset, 0);
        }
    }
}

void GatewayAssembly::mirrorBridgesOnce() {
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.mirrorCount <= 0) continue;
        auto it = m_transports.find(bridge.server);
        if (it == m_transports.end()) continue;
        auto* server = it->second.get();

        core::RegisterWords values(size_t(bridge.mirrorCount), 0);
        for (auto const& [address, dp] : m_bridgeMirrors[size_t(i)]) {
            int const idx = address - bridge.mirrorStart;
            if (idx >= 0 && idx < bridge.mirrorCount) {
                values[size_t(idx)] = std::uint16_t(dp::toUInt64(dp->value()));
            }
        }

        transport::WriteBatch batch;
        batch.table = core::RegisterTable::HoldingRegister;
        batch.startAddress = bridge.mirrorStart + bridge.offset;
        batch.values = std::move(values);

        sched::RequestTag tag;
        tag.moduleId = "bridge.mirror." + bridge.server;
        tag.priority = sched::Priority::Low;
        tag.coalesce = true;
        server->scheduler().submitAsync(tag, [server, batch](sched::AsyncDone done) {
            server->writeAsync(batch, [done](transport::WriteResult result) mutable {
                done(result.ok);
            });
        });
    }
}

void GatewayAssembly::wireBindings(module::PollRange& poll,
                                   config::ConfigSchema const& schema,
                                   DpById const& byId,
                                   std::string const& transportId,
                                   transport::ReadRequest const& req) {
    for (auto const& dc : schema.datapoints) {
        if (!dc.hasSource) continue;
        if (dc.source.port != transportId) continue;
        if (tableFromString(dc.source.table) != req.table) continue;

        int const rc = dp::registerCountFor(dc.type);
        int const offset = dc.source.address - req.startAddress;
        if (offset < 0 || offset + rc > req.count) continue;

        auto itDp = byId.find(dc.id);
        if (itDp == byId.end()) continue;

        auto codec = m_codecs.find(dc.source.codec.empty()
            ? codec::BuiltinScalarCodec::idFor(dc.type)
            : dc.source.codec);
        if (!codec) continue;
        poll.bind(itDp->second, std::move(codec), offset);
    }
}

void GatewayAssembly::schedulePoll(PollTimer& pollTimer) {
    if (!m_started || !pollTimer.poll || !pollTimer.timer) return;

    pollTimer.timer->expires_after(std::chrono::milliseconds(pollTimer.poll->periodMs()));
    pollTimer.timer->async_wait([this, &pollTimer](auto const& ec) {
        if (ec || !m_started) return;
        pollTimer.poll->driveTick();
        schedulePoll(pollTimer);
    });
}

void GatewayAssembly::startMirrorPump() {
    int period = 100;
    bool any = false;
    for (auto const& bridge : m_bridges) {
        if (bridge.mirrorCount <= 0) continue;
        any = true;
        period = std::min(period, std::max(20, bridge.mirrorPeriodMs));
    }
    if (!any) return;
    if (!m_mirrorTimer) {
        m_mirrorTimer = std::make_unique<gateway_asio::steady_timer>(*m_io);
    }
    m_mirrorTimer->expires_after(std::chrono::milliseconds(period));
    m_mirrorTimer->async_wait([this, period](auto const& ec) {
        if (ec || !m_started || !m_mirrorTimer) return;
        mirrorBridgesOnce();
        startMirrorPump();
    });
}

void GatewayAssembly::scheduleSink(SinkTimer& sinkTimer) {
    if (!m_started || !sinkTimer.sink || !sinkTimer.timer) return;
    sinkTimer.timer->expires_after(std::chrono::milliseconds(sinkTimer.sink->tickPeriodMs()));
    sinkTimer.timer->async_wait([this, &sinkTimer](auto const& ec) {
        if (ec || !m_started) return;
        sinkTimer.sink->driveTick();
        scheduleSink(sinkTimer);
    });
}

} // namespace core::gateway
