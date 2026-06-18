// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioModbusRtuClient.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
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

// How many bytes follow the already-read [unitId][function] pair, given the
// function byte. For read responses the byte-count field must be read first
// (returns 1 here, then the caller reads `byteCount + 2` more). Returns -1 for
// an unrecognised function so the caller can fail the frame.
int remainingAfterFunction(std::uint8_t function) {
    if (function & 0x80u) return 3;                       // exception: code + CRC
    auto const base = std::uint8_t(function & 0x7Fu);
    if (base == 0x03 || base == 0x04) return 1;           // read: byte-count next
    if (base == 0x10) return 6;                           // write multi: 4 echo + CRC
    return -1;
}

// Blocking transaction for the synchronous Transport::read/writeBatch contract
// (Phase-1 submit path, never the io thread). Mirrors AsioModbusTcpClient::
// transact: write then staged reads recovered from the function code. No
// deadline — the async path (transactAsync) is the one the gateway drives.
std::vector<std::uint8_t> blockingTransact(gateway_asio::serial_port& serial,
                                           std::vector<std::uint8_t> const& request,
                                           std::string& error) {
    gateway_error_code ec;
    gateway_asio::write(serial, gateway_asio::buffer(request), ec);
    if (ec) { error = "Modbus RTU write failed: " + ec.message(); return {}; }

    std::vector<std::uint8_t> resp(2);
    gateway_asio::read(serial, gateway_asio::buffer(resp.data(), 2), ec);
    if (ec) { error = "Modbus RTU read header failed: " + ec.message(); return {}; }

    auto const function = resp[1];
    auto const tail = remainingAfterFunction(function);
    if (tail < 0) { error = "unexpected Modbus RTU function"; return {}; }

    if ((function & 0x80u) || (function & 0x7Fu) == 0x10) {
        resp.resize(2 + std::size_t(tail));
        gateway_asio::read(serial, gateway_asio::buffer(resp.data() + 2, tail), ec);
        if (ec) { error = "Modbus RTU read tail failed: " + ec.message(); return {}; }
        return resp;
    }

    resp.resize(3);
    gateway_asio::read(serial, gateway_asio::buffer(resp.data() + 2, 1), ec);
    if (ec) { error = "Modbus RTU read byte-count failed: " + ec.message(); return {}; }
    auto const byteCount = resp[2];
    resp.resize(3 + std::size_t(byteCount) + 2);
    gateway_asio::read(serial, gateway_asio::buffer(resp.data() + 3, byteCount + 2), ec);
    if (ec) { error = "Modbus RTU read payload failed: " + ec.message(); return {}; }
    return resp;
}

} // namespace

AsioModbusRtuClient::AsioModbusRtuClient(config::TransportConfig cfg,
                                         gateway_asio::io_context& io)
    : m_cfg(std::move(cfg))
    , m_io(&io)
    , m_serial(io) {
    // RTU is strictly half-duplex with no transaction id: overlapping requests
    // on the line would consume each other's replies. Force single in-flight
    // regardless of the configured scheduler (unlike TCP, which disambiguates
    // concurrent transactions by MBAP transaction id).
    m_cfg.scheduler.maxInflight = 1;
    m_scheduler = sched::makeScheduler(m_cfg.scheduler);
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_io);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });
}

AsioModbusRtuClient::~AsioModbusRtuClient() {
    disconnect();
}

std::string AsioModbusRtuClient::id() const {
    return m_cfg.id;
}

transport::TransportKind AsioModbusRtuClient::kind() const {
    return transport::TransportKind::ModbusRtu;
}

transport::ConnectionState AsioModbusRtuClient::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> AsioModbusRtuClient::applySerialOptions() {
    using sp = gateway_asio::serial_port_base;
    gateway_error_code ec;

    m_serial.set_option(sp::baud_rate(unsigned(m_cfg.baudRate)), ec);
    if (ec) return std::unexpected("set baud_rate failed: " + ec.message());

    m_serial.set_option(sp::character_size(unsigned(m_cfg.dataBits)), ec);
    if (ec) return std::unexpected("set character_size failed: " + ec.message());

    auto const stop = m_cfg.stopBits == 2 ? sp::stop_bits::two : sp::stop_bits::one;
    m_serial.set_option(sp::stop_bits(stop), ec);
    if (ec) return std::unexpected("set stop_bits failed: " + ec.message());

    auto parity = sp::parity::none;
    if (m_cfg.parity == "even") parity = sp::parity::even;
    else if (m_cfg.parity == "odd") parity = sp::parity::odd;
    m_serial.set_option(sp::parity(parity), ec);
    if (ec) return std::unexpected("set parity failed: " + ec.message());

    m_serial.set_option(sp::flow_control(sp::flow_control::none), ec);
    if (ec) return std::unexpected("set flow_control failed: " + ec.message());

    return {};
}

std::expected<void, std::string> AsioModbusRtuClient::connect() {
    std::lock_guard lk(m_portMtx);
    m_state.store(transport::ConnectionState::Connecting, std::memory_order_release);

    closePortLocked();
    gateway_error_code ec;
    m_serial.open(m_cfg.portName, ec);
    if (ec) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("open serial '" + m_cfg.portName + "' failed: " + ec.message());
    }

    if (auto applied = applySerialOptions(); !applied) {
        closePortLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("serial '" + m_cfg.portName + "': " + applied.error());
    }

    m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
    return {};
}

void AsioModbusRtuClient::disconnect() {
    std::lock_guard lk(m_portMtx);
    if (m_scheduler) m_scheduler->stopAsync();
    closePortLocked();
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
}

sched::RequestScheduler& AsioModbusRtuClient::scheduler() {
    return *m_scheduler;
}

transport::ReadResult AsioModbusRtuClient::read(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (state() != transport::ConnectionState::Connected) {
        out.errorMessage = "Modbus RTU port is not connected";
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

    auto request = nmbs::buildRtuReadRequest(std::uint8_t(m_cfg.slaveId), function,
                                             std::uint16_t(req.startAddress),
                                             std::uint16_t(req.count));

    std::lock_guard lk(m_portMtx);
    if (!m_serial.is_open()) {
        out.errorMessage = "Modbus RTU port is closed";
        return out;
    }
    auto response = blockingTransact(m_serial, request, error);
    if (!error.empty()) {
        closePortLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        out.errorMessage = error;
        return out;
    }
    if (!nmbs::parseRtuReadResponse(response, std::uint8_t(m_cfg.slaveId), function,
                                    out.values, error)) {
        out.errorMessage = error;
        return out;
    }
    out.ok = true;
    cacheHoldingRegisters(req.startAddress, out.values);
    return out;
}

transport::WriteResult AsioModbusRtuClient::writeBatch(transport::WriteBatch const& batch) {
    if (state() != transport::ConnectionState::Connected) {
        return {false, "Modbus RTU port is not connected"};
    }
    if (batch.table != core::RegisterTable::HoldingRegister) {
        return {false, "unsupported Modbus write table"};
    }
    if (batch.startAddress < 0 || batch.values.empty() || batch.values.size() > 123) {
        return {false, "invalid Modbus write range"};
    }

    auto request = nmbs::buildRtuWriteMultipleRegistersRequest(
        std::uint8_t(m_cfg.slaveId), std::uint16_t(batch.startAddress), batch.values);

    std::lock_guard lk(m_portMtx);
    if (!m_serial.is_open()) {
        return {false, "Modbus RTU port is closed"};
    }
    std::string error;
    auto response = blockingTransact(m_serial, request, error);
    if (!error.empty()) {
        closePortLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return {false, error};
    }
    if (!nmbs::parseRtuWriteMultipleRegistersResponse(
            response, std::uint8_t(m_cfg.slaveId), std::uint16_t(batch.startAddress),
            std::uint16_t(batch.values.size()), error)) {
        return {false, error};
    }
    return {true, {}};
}

void AsioModbusRtuClient::readAsync(transport::ReadRequest const& req, ReadDone done) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (state() != transport::ConnectionState::Connected) {
        out.errorMessage = "Modbus RTU port is not connected";
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

    auto request = std::make_shared<std::vector<std::uint8_t>>(
        nmbs::buildRtuReadRequest(std::uint8_t(m_cfg.slaveId), function,
                                  std::uint16_t(req.startAddress),
                                  std::uint16_t(req.count)));

    transactAsync(std::move(request),
        [this, req, function, done = std::move(done)](
            std::vector<std::uint8_t> response, std::string err) mutable {
            transport::ReadResult result;
            result.startAddress = req.startAddress;
            if (!err.empty()) {
                result.errorMessage = std::move(err);
                done(std::move(result));
                return;
            }
            std::string parseError;
            if (!nmbs::parseRtuReadResponse(response, std::uint8_t(m_cfg.slaveId),
                                            function, result.values, parseError)) {
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

void AsioModbusRtuClient::writeAsync(transport::WriteBatch const& batch, WriteDone done) {
    if (state() != transport::ConnectionState::Connected) {
        done({false, "Modbus RTU port is not connected"});
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

    auto request = std::make_shared<std::vector<std::uint8_t>>(
        nmbs::buildRtuWriteMultipleRegistersRequest(
            std::uint8_t(m_cfg.slaveId), std::uint16_t(batch.startAddress), batch.values));

    auto const startAddress = std::uint16_t(batch.startAddress);
    auto const count = std::uint16_t(batch.values.size());
    transactAsync(std::move(request),
        [this, startAddress, count, done = std::move(done)](
            std::vector<std::uint8_t> response, std::string err) mutable {
            if (!err.empty()) {
                done({false, std::move(err)});
                return;
            }
            std::string parseError;
            if (!nmbs::parseRtuWriteMultipleRegistersResponse(
                    response, std::uint8_t(m_cfg.slaveId), startAddress, count, parseError)) {
                done({false, parseError});
                return;
            }
            done({true, {}});
        });
}

// Staged async RTU transaction on the single gateway io thread: async_write →
// read [unit][func] → (by function) read the variable tail → assemble the full
// ADU. A `requestTimeoutMs` deadline timer races the I/O chain; whichever fires
// first wins (guarded by `finished`). A timeout/error closes the port so the
// next request starts clean. No call blocks the io thread on the device.
void AsioModbusRtuClient::transactAsync(
    std::shared_ptr<std::vector<std::uint8_t>> request,
    TransactDone done) {
    {
        std::lock_guard lk(m_portMtx);
        if (!m_serial.is_open()) {
            m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
            done({}, "Modbus RTU port is closed");
            return;
        }
    }

    struct Txn {
        std::shared_ptr<std::vector<std::uint8_t>> request;
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
        if (!err.empty()) failPortAsync(err);
        txn->done(std::move(response), std::move(err));
    };

    auto const timeoutMs = m_cfg.requestTimeoutMs > 0 ? m_cfg.requestTimeoutMs : 1000;
    txn->timer->expires_after(std::chrono::milliseconds(timeoutMs));
    txn->timer->async_wait([finish](gateway_error_code const& ec) {
        if (ec) return;  // cancelled by completion
        finish({}, "Modbus RTU request timeout");
    });

    // Read `n` more bytes into response[from..from+n), then continue.
    auto readMore = std::make_shared<std::function<void(std::size_t, std::size_t,
                                                        std::function<void()>)>>();
    *readMore = [this, txn, finish, readMore](std::size_t from, std::size_t n,
                                              std::function<void()> next) {
        txn->response.resize(from + n);
        gateway_asio::async_read(m_serial,
            gateway_asio::buffer(txn->response.data() + from, n),
            [txn, finish, next = std::move(next)](gateway_error_code ec, std::size_t) {
                if (txn->finished) return;
                if (ec) {
                    finish({}, "Modbus RTU read failed: " + ec.message());
                    return;
                }
                next();
            });
    };

    gateway_asio::async_write(m_serial, gateway_asio::buffer(*txn->request),
        [this, txn, finish, readMore](gateway_error_code ec, std::size_t) {
            if (txn->finished) return;
            if (ec) {
                finish({}, "Modbus RTU write failed: " + ec.message());
                return;
            }
            // Stage 1: [unitId][function]
            (*readMore)(0, 2, [txn, finish, readMore] {
                auto const function = txn->response[1];
                auto const tail = remainingAfterFunction(function);
                if (tail < 0) {
                    finish({}, "unexpected Modbus RTU function");
                    return;
                }
                if ((function & 0x80u) || (function & 0x7Fu) == 0x10) {
                    // Fixed-length tail (exception=3, write-multi echo=6).
                    (*readMore)(2, std::size_t(tail),
                                [txn, finish] { finish(std::move(txn->response), {}); });
                    return;
                }
                // Read response: byte-count then `byteCount + 2` (data + CRC).
                (*readMore)(2, 1, [txn, finish, readMore] {
                    auto const byteCount = txn->response[2];
                    (*readMore)(3, std::size_t(byteCount) + 2,
                                [txn, finish] { finish(std::move(txn->response), {}); });
                });
            });
        });
}

void AsioModbusRtuClient::failPortAsync(std::string const&) {
    std::lock_guard lk(m_portMtx);
    closePortLocked();
    m_state.store(transport::ConnectionState::Error, std::memory_order_release);
}

void AsioModbusRtuClient::cacheHoldingRegisters(int startAddress,
                                                core::RegisterWords const& values) {
    std::lock_guard lk(m_cacheMtx);
    for (std::size_t i = 0; i < values.size(); i++) {
        m_hrCache[startAddress + int(i)] = values[i];
    }
}

core::RegisterWords AsioModbusRtuClient::snapshotHoldingRegisters(int start, int count) const {
    core::RegisterWords out(count > 0 ? std::size_t(count) : 0, 0);
    if (count <= 0) return out;
    std::lock_guard lk(m_cacheMtx);
    for (int i = 0; i < count; i++) {
        auto it = m_hrCache.find(start + i);
        if (it != m_hrCache.end()) out[std::size_t(i)] = it->second;
    }
    return out;
}

void AsioModbusRtuClient::closePortLocked() {
    gateway_error_code ignored;
    if (m_serial.is_open()) {
        m_serial.cancel(ignored);
        m_serial.close(ignored);
    }
}

} // namespace core::gateway
