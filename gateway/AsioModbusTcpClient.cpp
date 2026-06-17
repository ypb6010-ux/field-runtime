// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioModbusTcpClient.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "nanomodbus.h"

namespace core::gateway {

namespace {

nmbs::Function functionForRead(core::RegisterTable table, std::string& error) {
    switch (table) {
        case core::RegisterTable::HoldingRegister:
            return nmbs::Function::ReadHoldingRegisters;
        case core::RegisterTable::InputRegister:
            return nmbs::Function::ReadInputRegisters;
        default:
            error = "unsupported Modbus read table";
            return nmbs::Function::ReadHoldingRegisters;
    }
}

} // namespace

AsioModbusTcpClient::AsioModbusTcpClient(config::TransportConfig cfg,
                                         gateway_asio::io_context& io)
    : m_cfg(std::move(cfg))
    , m_io(&io)
    , m_socket(io)
    , m_scheduler(sched::makeScheduler(m_cfg.scheduler)) {
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_io);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });
}

AsioModbusTcpClient::~AsioModbusTcpClient() {
    disconnect();
}

std::string AsioModbusTcpClient::id() const {
    return m_cfg.id;
}

transport::TransportKind AsioModbusTcpClient::kind() const {
    return transport::TransportKind::ModbusTcpClient;
}

transport::ConnectionState AsioModbusTcpClient::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> AsioModbusTcpClient::connect() {
    std::lock_guard lk(m_socketMtx);
    m_state.store(transport::ConnectionState::Connecting, std::memory_order_release);

    gateway_error_code ec;
    gateway_asio::ip::tcp::resolver resolver(*m_io);
    auto endpoints = resolver.resolve(m_cfg.host, std::to_string(m_cfg.port), ec);
    if (ec) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("resolve " + m_cfg.host + ":" + std::to_string(m_cfg.port)
            + " failed: " + ec.message());
    }

    closeSocketLocked();
    m_socket = gateway_asio::ip::tcp::socket(*m_io);
    gateway_asio::connect(m_socket, endpoints, ec);
    if (ec) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("connect " + m_cfg.host + ":" + std::to_string(m_cfg.port)
            + " failed: " + ec.message());
    }

    m_socket.set_option(gateway_asio::ip::tcp::no_delay(true), ec);
    m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
    return {};
}

void AsioModbusTcpClient::disconnect() {
    std::lock_guard lk(m_socketMtx);
    if (m_scheduler) m_scheduler->stopAsync();
    closeSocketLocked();
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
}

sched::RequestScheduler& AsioModbusTcpClient::scheduler() {
    return *m_scheduler;
}

transport::ReadResult AsioModbusTcpClient::read(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (state() != transport::ConnectionState::Connected) {
        out.errorMessage = "Modbus TCP client is not connected";
        return out;
    }
    if (req.count <= 0 || req.count > 125 || req.startAddress < 0) {
        out.errorMessage = "invalid Modbus read range";
        return out;
    }

    std::string error;
    auto const function = functionForRead(req.table, error);
    if (!error.empty()) {
        out.errorMessage = error;
        return out;
    }

    auto const tx = nextTransactionId();
    auto request = nmbs::buildReadRequest(nmbs::ReadRequest{
        tx,
        std::uint8_t(m_cfg.slaveId),
        function,
        std::uint16_t(req.startAddress),
        std::uint16_t(req.count)
    });
    auto response = transact(request, error);
    if (!error.empty()) {
        out.errorMessage = error;
        return out;
    }

    if (!nmbs::parseReadResponse(response, tx, function, out.values, error)) {
        out.errorMessage = error;
        return out;
    }

    out.ok = true;
    return out;
}

transport::WriteResult AsioModbusTcpClient::writeBatch(transport::WriteBatch const& batch) {
    if (state() != transport::ConnectionState::Connected) {
        return {false, "Modbus TCP client is not connected"};
    }
    if (batch.table != core::RegisterTable::HoldingRegister) {
        return {false, "unsupported Modbus write table"};
    }
    if (batch.startAddress < 0 || batch.values.empty() || batch.values.size() > 123) {
        return {false, "invalid Modbus write range"};
    }

    auto const tx = nextTransactionId();
    auto request = nmbs::buildWriteMultipleRegistersRequest(
        nmbs::WriteMultipleRegistersRequest{
            tx,
            std::uint8_t(m_cfg.slaveId),
            std::uint16_t(batch.startAddress),
            batch.values
        });

    std::string error;
    auto response = transact(request, error);
    if (!error.empty()) return {false, error};

    if (!nmbs::parseWriteMultipleRegistersResponse(
            response,
            tx,
            std::uint16_t(batch.startAddress),
            std::uint16_t(batch.values.size()),
            error)) {
        return {false, error};
    }
    return {true, {}};
}

std::vector<std::uint8_t> AsioModbusTcpClient::transact(
    std::vector<std::uint8_t> const& request,
    std::string& error) {
    std::lock_guard lk(m_socketMtx);
    if (!m_socket.is_open()) {
        error = "Modbus TCP socket is closed";
        m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
        return {};
    }

    gateway_error_code ec;
    gateway_asio::write(m_socket, gateway_asio::buffer(request), ec);
    if (ec) {
        error = "Modbus TCP write failed: " + ec.message();
        closeSocketLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {};
    }

    std::array<std::uint8_t, 7> header{};
    gateway_asio::read(m_socket, gateway_asio::buffer(header), ec);
    if (ec) {
        error = "Modbus TCP read header failed: " + ec.message();
        closeSocketLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {};
    }

    auto const length = std::uint16_t((std::uint16_t(header[4]) << 8) | header[5]);
    if (length < 2 || length > 260) {
        error = "invalid Modbus TCP response length";
        closeSocketLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {};
    }

    std::vector<std::uint8_t> response(header.begin(), header.end());
    response.resize(6 + length);
    gateway_asio::read(m_socket, gateway_asio::buffer(response.data() + 7, length - 1), ec);
    if (ec) {
        error = "Modbus TCP read payload failed: " + ec.message();
        closeSocketLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {};
    }
    return response;
}

void AsioModbusTcpClient::readAsync(transport::ReadRequest const& req, ReadDone done) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (state() != transport::ConnectionState::Connected) {
        out.errorMessage = "Modbus TCP client is not connected";
        done(std::move(out));
        return;
    }
    if (req.count <= 0 || req.count > 125 || req.startAddress < 0) {
        out.errorMessage = "invalid Modbus read range";
        done(std::move(out));
        return;
    }

    std::string error;
    auto const function = functionForRead(req.table, error);
    if (!error.empty()) {
        out.errorMessage = error;
        done(std::move(out));
        return;
    }

    auto const tx = nextTransactionId();
    auto request = std::make_shared<std::vector<std::uint8_t>>(
        nmbs::buildReadRequest(nmbs::ReadRequest{
            tx,
            std::uint8_t(m_cfg.slaveId),
            function,
            std::uint16_t(req.startAddress),
            std::uint16_t(req.count)
        }));

    transactAsync(std::move(request),
        [this, req, tx, function, done = std::move(done)](
            std::vector<std::uint8_t> response, std::string err) mutable {
            transport::ReadResult result;
            result.startAddress = req.startAddress;
            if (!err.empty()) {
                result.errorMessage = std::move(err);
                done(std::move(result));
                return;
            }
            std::string parseError;
            if (!nmbs::parseReadResponse(response, tx, function, result.values, parseError)) {
                result.errorMessage = parseError;
                done(std::move(result));
                return;
            }
            result.ok = true;
            if (req.table == core::RegisterTable::HoldingRegister) {
                cacheHoldingRegisters(req.startAddress, result.values);
            }
            done(std::move(result));
        });
}

void AsioModbusTcpClient::writeAsync(transport::WriteBatch const& batch, WriteDone done) {
    if (state() != transport::ConnectionState::Connected) {
        done({false, "Modbus TCP client is not connected"});
        return;
    }
    if (batch.table != core::RegisterTable::HoldingRegister) {
        done({false, "unsupported Modbus write table"});
        return;
    }
    if (batch.startAddress < 0 || batch.values.empty() || batch.values.size() > 123) {
        done({false, "invalid Modbus write range"});
        return;
    }

    auto const tx = nextTransactionId();
    auto request = std::make_shared<std::vector<std::uint8_t>>(
        nmbs::buildWriteMultipleRegistersRequest(
            nmbs::WriteMultipleRegistersRequest{
                tx,
                std::uint8_t(m_cfg.slaveId),
                std::uint16_t(batch.startAddress),
                batch.values
            }));

    auto const startAddress = std::uint16_t(batch.startAddress);
    auto const count = std::uint16_t(batch.values.size());
    transactAsync(std::move(request),
        [tx, startAddress, count, done = std::move(done)](
            std::vector<std::uint8_t> response, std::string err) mutable {
            if (!err.empty()) {
                done({false, std::move(err)});
                return;
            }
            std::string parseError;
            if (!nmbs::parseWriteMultipleRegistersResponse(
                    response, tx, startAddress, count, parseError)) {
                done({false, parseError});
                return;
            }
            done({true, {}});
        });
}

// Fully async request/response on the single gateway io thread: async_write →
// async_read(header) → async_read(payload), bounded by a `requestTimeoutMs`
// deadline timer. Whichever of the I/O chain or the deadline fires first wins
// (guarded by `finished`); a timeout closes the socket so the next request
// starts clean. No call ever blocks the io thread waiting on the device.
void AsioModbusTcpClient::transactAsync(
    std::shared_ptr<std::vector<std::uint8_t>> request,
    TransactDone done) {
    {
        std::lock_guard lk(m_socketMtx);
        if (!m_socket.is_open()) {
            m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
            done({}, "Modbus TCP socket is closed");
            return;
        }
    }

    struct Txn {
        std::shared_ptr<std::vector<std::uint8_t>> request;
        std::array<std::uint8_t, 7> header{};
        std::vector<std::uint8_t> response;
        TransactDone done;
        std::shared_ptr<gateway_asio::steady_timer> timer;
        bool finished = false;
    };

    auto txn = std::make_shared<Txn>();
    txn->request = std::move(request);
    txn->done = std::move(done);
    txn->timer = std::make_shared<gateway_asio::steady_timer>(*m_io);

    auto finish = [this, txn](std::vector<std::uint8_t> response, std::string err) {
        if (txn->finished) return;
        txn->finished = true;
        gateway_error_code ignored;
        txn->timer->cancel(ignored);
        if (!err.empty()) failSocketAsync(err);
        txn->done(std::move(response), std::move(err));
    };

    auto const timeoutMs = m_cfg.requestTimeoutMs > 0 ? m_cfg.requestTimeoutMs : 1000;
    txn->timer->expires_after(std::chrono::milliseconds(timeoutMs));
    txn->timer->async_wait([finish](gateway_error_code const& ec) {
        if (ec) return;  // cancelled by completion
        finish({}, "Modbus TCP request timeout");
    });

    gateway_asio::async_write(m_socket, gateway_asio::buffer(*txn->request),
        [this, txn, finish](gateway_error_code ec, std::size_t) {
            if (txn->finished) return;
            if (ec) {
                finish({}, "Modbus TCP write failed: " + ec.message());
                return;
            }
            gateway_asio::async_read(m_socket, gateway_asio::buffer(txn->header),
                [this, txn, finish](gateway_error_code ec, std::size_t) {
                    if (txn->finished) return;
                    if (ec) {
                        finish({}, "Modbus TCP read header failed: " + ec.message());
                        return;
                    }
                    auto const length = std::uint16_t(
                        (std::uint16_t(txn->header[4]) << 8) | txn->header[5]);
                    if (length < 2 || length > 260) {
                        finish({}, "invalid Modbus TCP response length");
                        return;
                    }
                    txn->response.assign(txn->header.begin(), txn->header.end());
                    txn->response.resize(6 + length);
                    gateway_asio::async_read(m_socket,
                        gateway_asio::buffer(txn->response.data() + 7, length - 1),
                        [txn, finish](gateway_error_code ec, std::size_t) {
                            if (txn->finished) return;
                            if (ec) {
                                finish({}, "Modbus TCP read payload failed: " + ec.message());
                                return;
                            }
                            finish(std::move(txn->response), {});
                        });
                });
        });
}

void AsioModbusTcpClient::failSocketAsync(std::string const&) {
    std::lock_guard lk(m_socketMtx);
    closeSocketLocked();
    m_state.store(transport::ConnectionState::Error, std::memory_order_release);
}

void AsioModbusTcpClient::cacheHoldingRegisters(int startAddress,
                                                core::RegisterWords const& values) {
    std::lock_guard lk(m_cacheMtx);
    for (std::size_t i = 0; i < values.size(); i++) {
        m_hrCache[startAddress + int(i)] = values[i];
    }
}

core::RegisterWords AsioModbusTcpClient::snapshotHoldingRegisters(int start, int count) const {
    core::RegisterWords out(count > 0 ? std::size_t(count) : 0, 0);
    if (count <= 0) return out;
    std::lock_guard lk(m_cacheMtx);
    for (int i = 0; i < count; i++) {
        auto it = m_hrCache.find(start + i);
        if (it != m_hrCache.end()) out[std::size_t(i)] = it->second;
    }
    return out;
}

std::uint16_t AsioModbusTcpClient::nextTransactionId() {
    auto next = m_transactionId.fetch_add(1, std::memory_order_acq_rel);
    if (next == 0) next = m_transactionId.fetch_add(1, std::memory_order_acq_rel);
    return next;
}

void AsioModbusTcpClient::closeSocketLocked() {
    gateway_error_code ignored;
    if (m_socket.is_open()) {
        m_socket.shutdown(gateway_asio::ip::tcp::socket::shutdown_both, ignored);
        m_socket.close(ignored);
    }
}

} // namespace core::gateway
