// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "ControlSocket.h"

#include <algorithm>
#include <cctype>
#include <istream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "GatewayJson.h"

namespace core::gateway {

namespace {

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string transportKindText(transport::TransportKind kind) {
    switch (kind) {
        case transport::TransportKind::ModbusTcpClient: return "ModbusTcpClient";
        case transport::TransportKind::ModbusTcpServer: return "ModbusTcpServer";
        case transport::TransportKind::ModbusRtu: return "ModbusRtu";
        case transport::TransportKind::OpcUaClient: return "OpcUaClient";
        case transport::TransportKind::MqttClient: return "MqttClient";
        case transport::TransportKind::MqttPahoClient: return "MqttPahoClient";
        case transport::TransportKind::S7Client: return "S7Client";
    }
    return "Unknown";
}

std::string connectionStateText(transport::ConnectionState state) {
    switch (state) {
        case transport::ConnectionState::Disconnected: return "Disconnected";
        case transport::ConnectionState::Connecting: return "Connecting";
        case transport::ConnectionState::Connected: return "Connected";
        case transport::ConnectionState::Error: return "Error";
    }
    return "Unknown";
}

std::string circuitStateText(sched::CircuitState state) {
    switch (state) {
        case sched::CircuitState::Closed: return "Closed";
        case sched::CircuitState::Open: return "Open";
        case sched::CircuitState::HalfOpen: return "HalfOpen";
    }
    return "Unknown";
}

void appendStatsJson(std::string& out, sched::SchedulerStats const& stats) {
    out += "{";
    out += "\"queueDepth\":" + std::to_string(stats.queueDepth);
    out += ",\"inflight\":" + std::to_string(stats.inflight);
    out += ",\"maxQueueDepth\":" + std::to_string(stats.maxQueueDepth);
    out += ",\"totalSubmitted\":" + std::to_string(stats.totalSubmitted);
    out += ",\"totalCompleted\":" + std::to_string(stats.totalCompleted);
    out += ",\"totalFailed\":" + std::to_string(stats.totalFailed);
    out += ",\"totalTimedOut\":" + std::to_string(stats.totalTimedOut);
    out += ",\"totalCancelled\":" + std::to_string(stats.totalCancelled);
    out += ",\"p50LatencyMs\":" + std::to_string(stats.p50LatencyMs);
    out += ",\"p99LatencyMs\":" + std::to_string(stats.p99LatencyMs);
    out += ",\"laneQueueDepth\":[";
    for (std::size_t i = 0; i < stats.laneQueueDepth.size(); i++) {
        if (i > 0) out.push_back(',');
        out += std::to_string(stats.laneQueueDepth[i]);
    }
    out += "],\"circuitState\":";
    json::appendString(out, circuitStateText(stats.circuitState));
    out += ",\"circuitErrorStreak\":" + std::to_string(stats.circuitErrorStreak);
    out += "}";
}

} // namespace

ControlSocket::ControlSocket(gateway_asio::io_context& io,
                             GatewayAssembly& gateway,
                             ControlConfig config)
    : m_io(&io)
    , m_gateway(&gateway)
    , m_config(std::move(config)) {}

ControlSocket::~ControlSocket() {
    stop();
}

void ControlSocket::start() {
    if (m_started) return;
    m_started = true;

    gateway_error_code ec;
    auto const address = gateway_asio::ip::make_address(m_config.listenAddress, ec);
    if (ec) {
        m_started = false;
        throw std::runtime_error("invalid control listen address: " + ec.message());
    }

    gateway_asio::ip::tcp::endpoint endpoint(
        address,
        static_cast<gateway_asio::ip::port_type>(m_config.listenPort));
    m_acceptor = std::make_unique<gateway_asio::ip::tcp::acceptor>(*m_io);
    m_acceptor->open(endpoint.protocol(), ec);
    if (!ec) m_acceptor->set_option(gateway_asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec) m_acceptor->bind(endpoint, ec);
    if (!ec) m_acceptor->listen(gateway_asio::socket_base::max_listen_connections, ec);
    if (ec) {
        m_acceptor.reset();
        m_started = false;
        throw std::runtime_error("control listen failed: " + ec.message());
    }

    startAccept();
}

void ControlSocket::stop() {
    m_started = false;
    gateway_error_code ignored;
    if (m_acceptor && m_acceptor->is_open()) {
        m_acceptor->cancel(ignored);
        m_acceptor->close(ignored);
    }
    m_acceptor.reset();
}

void ControlSocket::startAccept() {
    if (!m_started || !m_acceptor) return;
    auto socket = std::make_shared<TcpSocket>(*m_io);
    m_acceptor->async_accept(*socket, [this, socket](gateway_error_code const& ec) {
        if (!m_started) return;
        if (!ec) handleClient(socket);
        startAccept();
    });
}

void ControlSocket::handleClient(std::shared_ptr<TcpSocket> socket) {
    auto buffer = std::make_shared<gateway_asio::streambuf>();
    gateway_asio::async_read_until(*socket, *buffer, '\n',
        [this, socket, buffer](gateway_error_code const& ec, std::size_t) {
            if (ec) return;
            std::istream is(buffer.get());
            std::string line;
            std::getline(is, line);
            auto response = std::make_shared<std::string>(handleCommand(line));
            response->push_back('\n');
            gateway_asio::async_write(*socket, gateway_asio::buffer(*response),
                [socket, response](gateway_error_code const&, std::size_t) {
                    gateway_error_code ignored;
                    socket->shutdown(gateway_asio::ip::tcp::socket::shutdown_both, ignored);
                    socket->close(ignored);
                });
        });
}

std::string ControlSocket::handleCommand(std::string const& command) {
    auto const cmd = trim(command);
    if (cmd == "status") return statusJson();
    if (cmd == "live") return liveJson();
    if (cmd == "help") return "{\"commands\":[\"status\",\"live\",\"help\"]}";
    return "{\"error\":\"unknown command\"}";
}

std::string ControlSocket::statusJson() {
    auto transports = m_gateway->transportSnapshots();
    std::sort(transports.begin(), transports.end(), [](auto const& a, auto const& b) {
        return a.id < b.id;
    });

    std::string out = "{\"transports\":[";
    for (std::size_t i = 0; i < transports.size(); i++) {
        auto const& t = transports[i];
        if (i > 0) out.push_back(',');
        out += "{\"id\":";
        json::appendString(out, t.id);
        out += ",\"kind\":";
        json::appendString(out, transportKindText(t.kind));
        out += ",\"state\":";
        json::appendString(out, connectionStateText(t.state));
        out += ",\"stats\":";
        appendStatsJson(out, t.stats);
        out += "}";
    }
    out += "]}";
    return out;
}

std::string ControlSocket::liveJson() const {
    auto datapoints = m_gateway->datapointSnapshots();
    std::string out = "{\"datapoints\":[";
    for (std::size_t i = 0; i < datapoints.size(); i++) {
        auto const& dp = datapoints[i];
        if (i > 0) out.push_back(',');
        out += "{\"id\":";
        json::appendString(out, dp.id);
        out += ",\"value\":";
        out += json::value(dp.value);
        out += ",\"quality\":";
        json::appendString(out, json::dpState(dp.state));
        out += ",\"ts\":" + std::to_string(json::timestampMs(dp.timestamp));
        out += "}";
    }
    out += "]}";
    return out;
}

} // namespace core::gateway
