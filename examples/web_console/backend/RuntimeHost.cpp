// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "RuntimeHost.h"

#include <chrono>
#include <utility>

#include "GatewayAssembly.h"
#include "GatewayJson.h"

#include "core/base/RegisterTable.h"
#include "core/transport/Transport.h"
#include "core/transport/TransportTypes.h"

namespace wc {

namespace {

std::string kindStr(core::transport::TransportKind k) {
    using K = core::transport::TransportKind;
    switch (k) {
        case K::ModbusTcpClient: return "modbus_tcp_client";
        case K::ModbusTcpServer: return "modbus_tcp_server";
        case K::ModbusRtu:       return "modbus_rtu";
        case K::OpcUaClient:     return "opc_ua_client";
        case K::MqttClient:      return "mqtt_client";
        case K::MqttPahoClient:  return "mqtt_client";
        case K::S7Client:        return "s7_client";
    }
    return "unknown";
}

std::string connStr(core::transport::ConnectionState s) {
    using S = core::transport::ConnectionState;
    switch (s) {
        case S::Disconnected: return "disconnected";
        case S::Connecting:   return "connecting";
        case S::Connected:    return "connected";
        case S::Error:        return "error";
    }
    return "unknown";
}

} // namespace

RuntimeHost::RuntimeHost()
    : m_workGuard(gateway_asio::make_work_guard(m_io)) {}

RuntimeHost::~RuntimeHost() {
    stop();
}

bool RuntimeHost::start(std::string const& tomlPath) {
    if (m_running.load()) return true;
    m_assembly = std::make_unique<core::gateway::GatewayAssembly>(m_io);
    if (!m_assembly->load(tomlPath)) {
        m_assembly.reset();
        return false;
    }
    m_assembly->start();
    m_pumpTimer = std::make_unique<gateway_asio::steady_timer>(m_io);
    m_running.store(true, std::memory_order_release);
    schedulePump();
    m_thread = std::thread([this] { m_io.run(); });
    return true;
}

void RuntimeHost::stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    gateway_asio::post(m_io, [this] {
        if (m_pumpTimer) { gateway_error_code ec; m_pumpTimer->cancel(ec); }
        if (m_assembly) m_assembly->stop();
    });
    m_workGuard.reset();
    m_io.stop();
    if (m_thread.joinable()) m_thread.join();
    m_assembly.reset();
}

void RuntimeHost::schedulePump() {
    if (!m_pumpTimer) return;
    m_pumpTimer->expires_after(std::chrono::milliseconds(500));
    m_pumpTimer->async_wait([this](gateway_error_code const& ec) {
        if (ec || !m_running.load(std::memory_order_acquire) || !m_assembly) return;
        auto dps = m_assembly->datapointSnapshots();
        auto tps = m_assembly->transportSnapshots();
        std::vector<DpSnap> ds;
        ds.reserve(dps.size());
        for (auto const& d : dps) {
            ds.push_back({d.id, d.value, core::gateway::json::dpState(d.state),
                          core::gateway::json::timestampMs(d.timestamp)});
        }
        std::vector<TpSnap> ts;
        ts.reserve(tps.size());
        for (auto const& t : tps) ts.push_back({t.id, kindStr(t.kind), connStr(t.state)});
        {
            std::lock_guard lk(m_mtx);
            m_dps = std::move(ds);
            m_tps = std::move(ts);
        }
        schedulePump();
    });
}

std::vector<DpSnap> RuntimeHost::datapoints() const {
    std::lock_guard lk(m_mtx);
    return m_dps;
}

std::vector<TpSnap> RuntimeHost::transports() const {
    std::lock_guard lk(m_mtx);
    return m_tps;
}

bool RuntimeHost::write(std::string const& transportId, int startAddress,
                        std::vector<std::uint16_t> values,
                        std::function<void(bool, std::string)> done) {
    if (!m_running.load()) { done(false, "runtime not running"); return false; }
    gateway_asio::post(m_io,
        [this, transportId, startAddress, values = std::move(values),
         done = std::move(done)]() mutable {
            core::transport::WriteBatch b;
            b.table = core::RegisterTable::HoldingRegister;
            b.startAddress = startAddress;
            b.values = core::RegisterWords(values.begin(), values.end());
            bool known = m_assembly->writeTransportAsync(
                transportId, std::move(b),
                [done](core::transport::WriteResult r) { done(r.ok, r.errorMessage); });
            if (!known) done(false, "unknown transport");
        });
    return true;
}

} // namespace wc
