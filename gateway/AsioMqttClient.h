// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "GatewayAsio.h"

namespace core::gateway {

struct MqttNorthboundConfig {
    bool enable = false;
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string clientId = "field_gateway";
    int keepaliveS = 30;
    std::string topicPrefix = "field";
    int qos = 0;
    int publishIntervalMs = 0;
};

class AsioMqttClient {
public:
    AsioMqttClient(gateway_asio::io_context& io, MqttNorthboundConfig config);
    ~AsioMqttClient();

    void start();
    void stop();
    void publish(std::string topic, std::string payload, int qos = 0);

    bool connected() const;
    MqttNorthboundConfig const& config() const;

private:
    struct PendingPublish {
        std::string topic;
        std::string payload;
        int qos = 0;
    };

    void connect();
    void onConnected();
    void readPacketType();
    void readRemainingLength(std::uint8_t packetType,
                             std::uint32_t value = 0,
                             int multiplier = 1);
    void readPacketBody(std::uint8_t packetType, std::uint32_t remainingLength);
    void handlePacket(std::uint8_t packetType, std::vector<std::uint8_t> const& body);
    void sendConnect();
    void sendPing();
    void schedulePing();
    void scheduleReconnect();
    void failAndReconnect();
    void flushPending();
    void writePacket(std::vector<std::uint8_t> packet);
    void startWrite();
    void closeSocket();

    gateway_asio::io_context* m_io = nullptr;
    MqttNorthboundConfig m_config;
    gateway_asio::ip::tcp::socket m_socket;
    gateway_asio::ip::tcp::resolver m_resolver;
    std::unique_ptr<gateway_asio::steady_timer> m_pingTimer;
    std::unique_ptr<gateway_asio::steady_timer> m_reconnectTimer;
    std::deque<PendingPublish> m_pending;
    std::deque<std::vector<std::uint8_t>> m_writeQueue;
    bool m_started = false;
    bool m_connected = false;
    bool m_writing = false;
    int m_reconnectDelayMs = 500;
};

} // namespace core::gateway
