// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "ControlSocket.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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

std::vector<std::string> splitWords(std::string const& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return value;
}

std::string errorJson(std::string const& error) {
    std::string out = "{\"error\":";
    json::appendString(out, error);
    out += "}";
    return out;
}

std::string okJson() {
    return "{\"ok\":true}";
}

bool parseInt(std::string const& text, int& out) {
    try {
        std::size_t used = 0;
        out = std::stoi(text, &used, 0);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

bool parseU16(std::string const& text, std::uint16_t& out) {
    int value = 0;
    if (!parseInt(text, value)) return false;
    if (value < 0 || value > 0xFFFF) return false;
    out = std::uint16_t(value);
    return true;
}

bool parseTable(std::string const& text, core::RegisterTable& table) {
    auto const value = lower(text);
    if (value == "hr" || value == "holdingregister" || value == "holdingregisters") {
        table = core::RegisterTable::HoldingRegister;
        return true;
    }
    if (value == "ir" || value == "inputregister" || value == "inputregisters") {
        table = core::RegisterTable::InputRegister;
        return true;
    }
    if (value == "coil" || value == "coils") {
        table = core::RegisterTable::Coil;
        return true;
    }
    if (value == "di" || value == "discreteinput" || value == "discreteinputs") {
        table = core::RegisterTable::DiscreteInput;
        return true;
    }
    return false;
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
    auto authenticated = std::make_shared<bool>(false);
    readCommand(std::move(socket), std::move(buffer), std::move(authenticated));
}

void ControlSocket::readCommand(std::shared_ptr<TcpSocket> socket,
                                std::shared_ptr<gateway_asio::streambuf> buffer,
                                std::shared_ptr<bool> authenticated) {
    gateway_asio::async_read_until(*socket, *buffer, '\n',
        [this, socket, buffer, authenticated](gateway_error_code const& ec, std::size_t) {
            if (ec) return;
            std::istream is(buffer.get());
            std::string line;
            std::getline(is, line);
            handleCommand(line, *authenticated,
                [this, socket, buffer, authenticated](std::string response) {
                    writeResponse(socket, buffer, authenticated, std::move(response));
                });
        });
}

void ControlSocket::writeResponse(std::shared_ptr<TcpSocket> socket,
                                  std::shared_ptr<gateway_asio::streambuf> buffer,
                                  std::shared_ptr<bool> authenticated,
                                  std::string response) {
    auto shared = std::make_shared<std::string>(std::move(response));
    shared->push_back('\n');
    gateway_asio::async_write(*socket, gateway_asio::buffer(*shared),
        [this, socket, buffer, authenticated, shared](gateway_error_code const& ec,
                                                      std::size_t) {
            if (ec) return;
            readCommand(socket, buffer, authenticated);
        });
}

void ControlSocket::handleCommand(std::string const& command,
                                  bool& authenticated,
                                  std::function<void(std::string)> done) {
    auto const cmd = trim(command);
    auto const parts = splitWords(cmd);
    if (parts.empty()) {
        done(errorJson("empty command"));
        return;
    }

    auto const verb = lower(parts.front());
    if (verb == "status") {
        done(statusJson());
    } else if (verb == "live") {
        done(liveJson());
    } else if (verb == "help") {
        done("{\"commands\":[\"status\",\"live\",\"help\",\"auth <token>\","
             "\"forward <server> on|off\","
             "\"write <transport> <table> <addr> <value> [value...]\"]}");
    } else if (verb == "auth") {
        done(handleAuth(parts, authenticated));
    } else if (verb == "forward") {
        done(handleForward(parts, authenticated));
    } else if (verb == "write") {
        handleWrite(parts, authenticated, std::move(done));
    } else {
        done(errorJson("unknown command"));
    }
}

std::string ControlSocket::handleAuth(std::vector<std::string> const& parts,
                                      bool& authenticated) const {
    if (m_config.authToken.empty()) return errorJson("auth disabled");
    if (parts.size() != 2) return errorJson("usage: auth <token>");
    if (parts[1] != m_config.authToken) {
        authenticated = false;
        return errorJson("invalid token");
    }
    authenticated = true;
    return okJson();
}

std::string ControlSocket::handleForward(std::vector<std::string> const& parts,
                                         bool authenticated) {
    if (!authenticated) return errorJson("unauthorized");
    if (parts.size() != 3) return errorJson("usage: forward <server> on|off");
    auto const& server = parts[1];
    auto const state = lower(parts[2]);
    if (state != "on" && state != "off") return errorJson("forward expects on|off");
    if (!m_gateway->hasServerTransport(server)) return errorJson("unknown server");

    bool const enabled = state == "on";
    m_gateway->setServerForwardEnabled(server, enabled);

    std::string out = "{\"ok\":true,\"server\":";
    json::appendString(out, server);
    out += ",\"forward\":";
    out += enabled ? "true" : "false";
    out += "}";
    return out;
}

void ControlSocket::handleWrite(std::vector<std::string> const& parts,
                                bool authenticated,
                                std::function<void(std::string)> done) {
    if (!authenticated) {
        done(errorJson("unauthorized"));
        return;
    }
    if (parts.size() < 5) {
        done(errorJson("usage: write <transport> <table> <addr> <value> [value...]"));
        return;
    }

    auto const& transportId = parts[1];
    if (!m_gateway->hasTransport(transportId)) {
        done(errorJson("unknown transport"));
        return;
    }

    core::RegisterTable table = core::RegisterTable::HoldingRegister;
    if (!parseTable(parts[2], table)) {
        done(errorJson("unknown table"));
        return;
    }

    int address = 0;
    if (!parseInt(parts[3], address) || address < 0) {
        done(errorJson("invalid address"));
        return;
    }

    core::RegisterWords values;
    values.reserve(parts.size() - 4);
    for (std::size_t i = 4; i < parts.size(); i++) {
        std::uint16_t value = 0;
        if (!parseU16(parts[i], value)) {
            done(errorJson("invalid register value"));
            return;
        }
        values.push_back(value);
    }

    transport::WriteBatch batch;
    batch.table = table;
    batch.startAddress = address;
    batch.values = std::move(values);
    auto const count = batch.values.size();

    if (!m_gateway->writeTransportAsync(
            transportId,
            std::move(batch),
            [transportId, tableText = parts[2], address, count,
             done = std::move(done)](transport::WriteResult result) mutable {
                if (!result.ok) {
                    done(errorJson(result.errorMessage.empty()
                        ? "write failed"
                        : result.errorMessage));
                    return;
                }
                std::string out = "{\"ok\":true,\"transport\":";
                json::appendString(out, transportId);
                out += ",\"table\":";
                json::appendString(out, tableText);
                out += ",\"addr\":" + std::to_string(address);
                out += ",\"count\":" + std::to_string(count);
                out += "}";
                done(std::move(out));
            })) {
        done(errorJson("unknown transport"));
    }
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
