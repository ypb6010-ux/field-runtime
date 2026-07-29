// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
    bool publishTracked(std::string topic,
                        std::string payload,
                        int qos,
                        std::function<void(bool)> done);
    void setConnectedCallback(std::function<void()> callback);

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
    std::uint16_t nextPacketId();
    void failInflight();
    void handlePuback(std::vector<std::uint8_t> const& body);
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
    bool writePacket(std::vector<std::uint8_t> packet,
                     std::function<void(bool)> done = {});
    void failWriteQueue();
    void startWrite();
    void closeSocket();

    struct PacketWrite {
        std::vector<std::uint8_t> packet;
        std::function<void(bool)> done;
    };

    gateway_asio::io_context* m_io = nullptr;
    MqttNorthboundConfig m_config;
    gateway_asio::ip::tcp::socket m_socket;
    gateway_asio::ip::tcp::resolver m_resolver;
    std::unique_ptr<gateway_asio::steady_timer> m_pingTimer;
    std::unique_ptr<gateway_asio::steady_timer> m_reconnectTimer;
    std::unique_ptr<gateway_asio::steady_timer> m_connectTimer;
    std::deque<PendingPublish> m_pending;
    std::deque<PacketWrite> m_writeQueue;
    std::map<std::uint16_t, std::function<void(bool)>> m_inflight;
    std::function<void()> m_connectedCallback;
    bool m_started = false;
    bool m_connected = false;
    bool m_writing = false;
    bool m_waitingPingResponse = false;
    int m_reconnectDelayMs = 500;
    std::uint16_t m_nextPacketId = 0;
    std::uint64_t m_generation = 0;
};

} // namespace core::gateway
