// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioS7Client.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <snap7/snap7_libmain.h>

namespace core::gateway {

namespace {

std::string s7ErrorText(int rc) {
    char text[256] = {0};
    Cli_ErrorText(rc, text, int(sizeof(text)));
    return std::string(text);
}

} // namespace

AsioS7Client::AsioS7Client(config::TransportConfig cfg, gateway_asio::io_context& mainIo)
    : m_cfg(std::move(cfg))
    , m_mainIo(&mainIo)
    , m_scheduler(sched::makeScheduler(m_cfg.scheduler))
    , m_workGuard(gateway_asio::make_work_guard(m_workerIo)) {
    // Scheduler timing runs on the main io thread (where submitAsync is driven),
    // matching the Modbus / OPC UA clients.
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_mainIo);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });
    m_workerThread = std::thread([this] { m_workerIo.run(); });
}

AsioS7Client::~AsioS7Client() {
    if (m_scheduler) m_scheduler->stopAsync();
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    gateway_asio::post(m_workerIo, [this, done] {
        if (m_client) {
            Cli_Disconnect(m_client);
            Cli_Destroy(m_client);   // sets m_client to 0
        }
        done->set_value();
    });
    fut.wait();
    m_workGuard.reset();
    m_workerIo.stop();
    if (m_workerThread.joinable()) m_workerThread.join();
}

std::string AsioS7Client::id() const {
    return m_cfg.id;
}

transport::TransportKind AsioS7Client::kind() const {
    return transport::TransportKind::S7Client;
}

transport::ConnectionState AsioS7Client::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> AsioS7Client::connect() {
    m_state.store(transport::ConnectionState::Connecting, std::memory_order_release);

    auto result = std::make_shared<std::promise<std::expected<void, std::string>>>();
    auto fut = result->get_future();
    gateway_asio::post(m_workerIo, [this, result] {
        if (!m_client) m_client = Cli_Create();
        int rc = Cli_ConnectTo(m_client, m_cfg.host.c_str(), m_cfg.rack, m_cfg.slot);
        if (rc != 0) {
            m_state.store(transport::ConnectionState::Error, std::memory_order_release);
            result->set_value(std::unexpected(
                "S7 connect '" + m_cfg.host + "' (rack " + std::to_string(m_cfg.rack)
                + " slot " + std::to_string(m_cfg.slot) + ") failed: " + s7ErrorText(rc)));
            return;
        }
        m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
        result->set_value({});
    });

    auto const timeoutMs = m_cfg.connectTimeoutMs > 0 ? m_cfg.connectTimeoutMs : 5000;
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected(std::string("S7 connect timeout: ") + m_cfg.host);
    }
    return fut.get();
}

void AsioS7Client::disconnect() {
    if (m_scheduler) m_scheduler->stopAsync();
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    gateway_asio::post(m_workerIo, [this, done] {
        if (m_client) Cli_Disconnect(m_client);
        done->set_value();
    });
    fut.wait();
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
}

sched::RequestScheduler& AsioS7Client::scheduler() {
    return *m_scheduler;
}

transport::ReadResult AsioS7Client::readOnWorker(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (!m_client || state() != transport::ConnectionState::Connected) {
        out.errorMessage = "S7 client is not connected";
        return out;
    }
    if (req.count <= 0 || req.startAddress < 0) {
        out.errorMessage = "invalid S7 read range";
        return out;
    }

    // addr is a 16-bit WORD index (matching the datapoint engine / Modbus);
    // S7 areas are byte-addressed, so byte offset = addr * 2.
    int const byteStart = req.startAddress * 2;
    int const sizeBytes = req.count * 2;
    std::vector<std::uint8_t> buf(std::size_t(sizeBytes), 0);

    int rc;
    if (req.table == core::RegisterTable::HoldingRegister) {
        rc = Cli_DBRead(m_client, m_cfg.s7Db, byteStart, sizeBytes, buf.data());
    } else if (req.table == core::RegisterTable::InputRegister) {
        rc = Cli_EBRead(m_client, byteStart, sizeBytes, buf.data());
    } else {
        out.errorMessage = "unsupported S7 area";
        return out;
    }
    if (rc != 0) {
        out.errorMessage = "S7 read failed: " + s7ErrorText(rc);
        int connected = 0;
        Cli_GetConnected(m_client, connected);
        if (!connected) m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return out;
    }

    core::RegisterWords words(std::size_t(req.count), 0);
    for (int i = 0; i < req.count; i++) {
        words[std::size_t(i)] = std::uint16_t(
            (std::uint16_t(buf[std::size_t(i) * 2]) << 8) | buf[std::size_t(i) * 2 + 1]);
    }
    out.ok = true;
    out.values = std::move(words);
    return out;
}

transport::WriteResult AsioS7Client::writeOnWorker(transport::WriteBatch const& batch) {
    if (!m_client || state() != transport::ConnectionState::Connected) {
        return {false, "S7 client is not connected"};
    }
    if (batch.table != core::RegisterTable::HoldingRegister) {
        return {false, "unsupported S7 write area"};
    }
    if (batch.startAddress < 0 || batch.values.empty()) {
        return {false, "invalid S7 write range"};
    }

    std::vector<std::uint8_t> buf(batch.values.size() * 2, 0);
    for (std::size_t i = 0; i < batch.values.size(); i++) {
        buf[i * 2]     = std::uint8_t(batch.values[i] >> 8);    // big-endian
        buf[i * 2 + 1] = std::uint8_t(batch.values[i] & 0xFFu);
    }
    int rc = Cli_DBWrite(m_client, m_cfg.s7Db, batch.startAddress * 2, int(buf.size()), buf.data());
    if (rc != 0) {
        int connected = 0;
        Cli_GetConnected(m_client, connected);
        if (!connected) m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {false, "S7 write failed: " + s7ErrorText(rc)};
    }
    return {true, {}};
}

transport::ReadResult AsioS7Client::read(transport::ReadRequest const& req) {
    auto result = std::make_shared<std::promise<transport::ReadResult>>();
    auto fut = result->get_future();
    gateway_asio::post(m_workerIo, [this, req, result] {
        result->set_value(readOnWorker(req));
    });
    return fut.get();
}

transport::WriteResult AsioS7Client::writeBatch(transport::WriteBatch const& batch) {
    auto result = std::make_shared<std::promise<transport::WriteResult>>();
    auto fut = result->get_future();
    gateway_asio::post(m_workerIo, [this, batch, result] {
        result->set_value(writeOnWorker(batch));
    });
    return fut.get();
}

void AsioS7Client::readAsync(transport::ReadRequest const& req, ReadDone done) {
    if (state() != transport::ConnectionState::Connected) {
        transport::ReadResult out;
        out.startAddress = req.startAddress;
        out.errorMessage = "S7 client is not connected";
        done(std::move(out));
        return;
    }
    auto* mainIo = m_mainIo;
    gateway_asio::post(m_workerIo,
        [this, req, mainIo, done = std::move(done)]() mutable {
            auto result = readOnWorker(req);
            if (result.ok && req.table == core::RegisterTable::HoldingRegister) {
                cacheHoldingRegisters(req.startAddress, result.values);
            }
            gateway_asio::post(*mainIo,
                [done = std::move(done), result = std::move(result)]() mutable {
                    done(std::move(result));
                });
        });
}

void AsioS7Client::writeAsync(transport::WriteBatch const& batch, WriteDone done) {
    if (state() != transport::ConnectionState::Connected) {
        done({false, "S7 client is not connected"});
        return;
    }
    auto* mainIo = m_mainIo;
    gateway_asio::post(m_workerIo,
        [this, batch, mainIo, done = std::move(done)]() mutable {
            auto result = writeOnWorker(batch);
            gateway_asio::post(*mainIo,
                [done = std::move(done), result = std::move(result)]() mutable {
                    done(std::move(result));
                });
        });
}

void AsioS7Client::cacheHoldingRegisters(int startAddress, core::RegisterWords const& values) {
    std::lock_guard lk(m_cacheMtx);
    for (std::size_t i = 0; i < values.size(); i++) {
        m_hrCache[startAddress + int(i)] = values[i];
    }
}

core::RegisterWords AsioS7Client::snapshotHoldingRegisters(int start, int count) const {
    core::RegisterWords out(count > 0 ? std::size_t(count) : 0, 0);
    if (count <= 0) return out;
    std::lock_guard lk(m_cacheMtx);
    for (int i = 0; i < count; i++) {
        auto it = m_hrCache.find(start + i);
        if (it != m_hrCache.end()) out[std::size_t(i)] = it->second;
    }
    return out;
}

} // namespace core::gateway
