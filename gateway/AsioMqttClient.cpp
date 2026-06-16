// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioMqttClient.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace core::gateway {

namespace {

constexpr std::size_t kMaxPendingPublishes = 256;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(std::uint8_t(value >> 8));
    out.push_back(std::uint8_t(value & 0xFF));
}

void appendUtf8(std::vector<std::uint8_t>& out, std::string const& value) {
    appendU16(out, std::uint16_t(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void appendRemainingLength(std::vector<std::uint8_t>& out, std::uint32_t value) {
    do {
        std::uint8_t encoded = std::uint8_t(value % 128);
        value /= 128;
        if (value > 0) encoded |= 0x80;
        out.push_back(encoded);
    } while (value > 0);
}

std::vector<std::uint8_t> buildConnect(MqttNorthboundConfig const& cfg) {
    std::vector<std::uint8_t> variable;
    appendUtf8(variable, "MQTT");
    variable.push_back(4);
    variable.push_back(0x02);
    appendU16(variable, std::uint16_t(cfg.keepaliveS));
    appendUtf8(variable, cfg.clientId);

    std::vector<std::uint8_t> packet;
    packet.push_back(0x10);
    appendRemainingLength(packet, std::uint32_t(variable.size()));
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

std::vector<std::uint8_t> buildPublish(std::string const& topic,
                                       std::string const& payload) {
    std::vector<std::uint8_t> variable;
    appendUtf8(variable, topic);
    variable.insert(variable.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> packet;
    packet.push_back(0x30);
    appendRemainingLength(packet, std::uint32_t(variable.size()));
    packet.insert(packet.end(), variable.begin(), variable.end());
    return packet;
}

} // namespace

AsioMqttClient::AsioMqttClient(gateway_asio::io_context& io,
                               MqttNorthboundConfig config)
    : m_io(&io)
    , m_config(std::move(config))
    , m_socket(io)
    , m_resolver(io)
    , m_pingTimer(std::make_unique<gateway_asio::steady_timer>(io))
    , m_reconnectTimer(std::make_unique<gateway_asio::steady_timer>(io)) {
    if (m_config.keepaliveS <= 0) m_config.keepaliveS = 30;
    if (m_config.clientId.empty()) m_config.clientId = "field_gateway";
    if (m_config.topicPrefix.empty()) m_config.topicPrefix = "field";
    m_config.qos = 0;
}

AsioMqttClient::~AsioMqttClient() {
    stop();
}

void AsioMqttClient::start() {
    if (m_started) return;
    m_started = true;
    connect();
}

void AsioMqttClient::stop() {
    m_started = false;
    m_connected = false;
    m_writing = false;
    m_pending.clear();
    m_writeQueue.clear();
    if (m_pingTimer) m_pingTimer->cancel();
    if (m_reconnectTimer) m_reconnectTimer->cancel();
    closeSocket();
}

void AsioMqttClient::publish(std::string topic, std::string payload, int qos) {
    PendingPublish pending{std::move(topic), std::move(payload), qos};
    pending.qos = 0;
    if (!m_started || !m_connected) {
        if (m_pending.size() >= kMaxPendingPublishes) m_pending.pop_front();
        m_pending.push_back(std::move(pending));
        return;
    }
    writePacket(buildPublish(pending.topic, pending.payload));
}

bool AsioMqttClient::publishTracked(std::string topic,
                                    std::string payload,
                                    int qos,
                                    std::function<void(bool)> done) {
    (void)qos;
    if (!m_started || !m_connected || !m_socket.is_open()) return false;
    writePacket(buildPublish(topic, payload), std::move(done));
    return true;
}

void AsioMqttClient::setConnectedCallback(std::function<void()> callback) {
    m_connectedCallback = std::move(callback);
}

bool AsioMqttClient::connected() const {
    return m_connected;
}

MqttNorthboundConfig const& AsioMqttClient::config() const {
    return m_config;
}

void AsioMqttClient::connect() {
    if (!m_started) return;
    m_connected = false;
    closeSocket();

    m_resolver.async_resolve(
        m_config.host,
        std::to_string(m_config.port),
        [this](gateway_error_code const& ec,
               gateway_asio::ip::tcp::resolver::results_type endpoints) {
            if (!m_started) return;
            if (ec) {
                scheduleReconnect();
                return;
            }
            gateway_asio::async_connect(m_socket, endpoints,
                [this](gateway_error_code const& connectEc,
                       gateway_asio::ip::tcp::endpoint const&) {
                    if (!m_started) return;
                    if (connectEc) {
                        scheduleReconnect();
                        return;
                    }
                    sendConnect();
                    readPacketType();
                });
        });
}

void AsioMqttClient::onConnected() {
    m_connected = true;
    m_reconnectDelayMs = 500;
    flushPending();
    if (m_connectedCallback) m_connectedCallback();
    schedulePing();
}

void AsioMqttClient::readPacketType() {
    auto byte = std::make_shared<std::uint8_t>(0);
    gateway_asio::async_read(m_socket, gateway_asio::buffer(byte.get(), 1),
        [this, byte](gateway_error_code const& ec, std::size_t) {
            if (!m_started) return;
            if (ec) {
                failAndReconnect();
                return;
            }
            readRemainingLength(*byte);
        });
}

void AsioMqttClient::readRemainingLength(std::uint8_t packetType,
                                         std::uint32_t value,
                                         int multiplier) {
    auto byte = std::make_shared<std::uint8_t>(0);
    gateway_asio::async_read(m_socket, gateway_asio::buffer(byte.get(), 1),
        [this, byte, packetType, value, multiplier](gateway_error_code const& ec,
                                                    std::size_t) {
            if (!m_started) return;
            if (ec) {
                failAndReconnect();
                return;
            }
            auto nextValue = value + std::uint32_t((*byte & 127) * multiplier);
            if ((*byte & 128) != 0) {
                if (multiplier > 128 * 128) {
                    failAndReconnect();
                    return;
                }
                readRemainingLength(packetType, nextValue, multiplier * 128);
                return;
            }
            readPacketBody(packetType, nextValue);
        });
}

void AsioMqttClient::readPacketBody(std::uint8_t packetType,
                                    std::uint32_t remainingLength) {
    auto body = std::make_shared<std::vector<std::uint8_t>>(remainingLength);
    if (remainingLength == 0) {
        handlePacket(packetType, *body);
        readPacketType();
        return;
    }
    gateway_asio::async_read(m_socket, gateway_asio::buffer(*body),
        [this, packetType, body](gateway_error_code const& ec, std::size_t) {
            if (!m_started) return;
            if (ec) {
                failAndReconnect();
                return;
            }
            handlePacket(packetType, *body);
            readPacketType();
        });
}

void AsioMqttClient::handlePacket(std::uint8_t packetType,
                                  std::vector<std::uint8_t> const& body) {
    auto const type = packetType >> 4;
    if (type == 2) {
        if (body.size() == 2 && body[0] == 0 && body[1] == 0) {
            onConnected();
        } else {
            failAndReconnect();
        }
    } else if (type == 13) {
        // PINGRESP: liveness confirmed; next ping is already timer-driven.
    }
}

void AsioMqttClient::sendConnect() {
    writePacket(buildConnect(m_config));
}

void AsioMqttClient::sendPing() {
    if (!m_started || !m_connected) return;
    writePacket({0xC0, 0x00});
    schedulePing();
}

void AsioMqttClient::schedulePing() {
    if (!m_started || !m_connected || !m_pingTimer) return;
    int const period = std::max(1, m_config.keepaliveS / 2);
    m_pingTimer->expires_after(std::chrono::seconds(period));
    m_pingTimer->async_wait([this](gateway_error_code const& ec) {
        if (!ec) sendPing();
    });
}

void AsioMqttClient::scheduleReconnect() {
    if (!m_started || !m_reconnectTimer) return;
    m_connected = false;
    m_writing = false;
    m_writeQueue.clear();
    closeSocket();
    auto const delay = m_reconnectDelayMs;
    m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, 10000);
    m_reconnectTimer->expires_after(std::chrono::milliseconds(delay));
    m_reconnectTimer->async_wait([this](gateway_error_code const& ec) {
        if (!ec) connect();
    });
}

void AsioMqttClient::failAndReconnect() {
    if (!m_started) return;
    scheduleReconnect();
}

void AsioMqttClient::flushPending() {
    while (m_connected && !m_pending.empty()) {
        auto pending = std::move(m_pending.front());
        m_pending.pop_front();
        writePacket(buildPublish(pending.topic, pending.payload));
    }
}

void AsioMqttClient::writePacket(std::vector<std::uint8_t> packet,
                                 std::function<void(bool)> done) {
    if (!m_started || !m_socket.is_open()) return;
    m_writeQueue.push_back(PacketWrite{std::move(packet), std::move(done)});
    if (!m_writing) startWrite();
}

void AsioMqttClient::startWrite() {
    if (!m_started || m_writing || m_writeQueue.empty() || !m_socket.is_open()) return;
    m_writing = true;
    auto& item = m_writeQueue.front();
    gateway_asio::async_write(m_socket, gateway_asio::buffer(item.packet),
        [this](gateway_error_code const& ec, std::size_t) {
            m_writing = false;
            if (ec) {
                if (!m_writeQueue.empty() && m_writeQueue.front().done) {
                    m_writeQueue.front().done(false);
                }
                if (m_started) failAndReconnect();
                return;
            }
            if (!m_writeQueue.empty() && m_writeQueue.front().done) {
                m_writeQueue.front().done(true);
            }
            if (!m_writeQueue.empty()) m_writeQueue.pop_front();
            startWrite();
        });
}

void AsioMqttClient::closeSocket() {
    gateway_error_code ignored;
    if (m_socket.is_open()) {
        m_socket.shutdown(gateway_asio::ip::tcp::socket::shutdown_both, ignored);
        m_socket.close(ignored);
    }
    m_socket = gateway_asio::ip::tcp::socket(*m_io);
}

} // namespace core::gateway
