// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioModbusTcpServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <utility>

#include "nanomodbus.h"

#include "core/bus/BusEvents.h"

namespace core::gateway {

namespace {

std::uint16_t getU16(std::span<std::uint8_t const> data, std::size_t pos) {
    return std::uint16_t((std::uint16_t(data[pos]) << 8) | data[pos + 1]);
}

core::RegisterTable tableForReadFunction(std::uint8_t function) {
    return function == 0x04 ? core::RegisterTable::InputRegister
                            : core::RegisterTable::HoldingRegister;
}

} // namespace

AsioModbusTcpServer::AsioModbusTcpServer(config::TransportConfig cfg,
                                         gateway_asio::io_context& io,
                                         bus::EventBus& bus)
    : m_cfg(std::move(cfg))
    , m_io(&io)
    , m_bus(&bus)
    , m_scheduler(sched::makeScheduler(m_cfg.scheduler)) {
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_io);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });

    std::lock_guard lk(m_mtx);
    for (auto const& r : m_cfg.listenRanges) {
        ensureRangeLocked(r.table, r.startAddress, r.size);
    }
    ensureRangeLocked(core::RegisterTable::HoldingRegister, 0, 128);
    ensureRangeLocked(core::RegisterTable::InputRegister, 0, 128);
}

AsioModbusTcpServer::~AsioModbusTcpServer() {
    disconnect();
}

std::string AsioModbusTcpServer::id() const {
    return m_cfg.id;
}

transport::TransportKind AsioModbusTcpServer::kind() const {
    return transport::TransportKind::ModbusTcpServer;
}

transport::ConnectionState AsioModbusTcpServer::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> AsioModbusTcpServer::connect() {
    std::lock_guard lk(m_mtx);
    gateway_error_code ec;
    auto const address = m_cfg.listenAddress.empty()
        ? gateway_asio::ip::address_v4::any()
        : gateway_asio::ip::make_address(m_cfg.listenAddress, ec);
    if (ec) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("invalid listen address: " + ec.message());
    }

    gateway_asio::ip::tcp::endpoint endpoint(
        address,
        static_cast<gateway_asio::ip::port_type>(m_cfg.listenPort));
    m_acceptor = std::make_unique<gateway_asio::ip::tcp::acceptor>(*m_io);
    m_acceptor->open(endpoint.protocol(), ec);
    if (!ec) m_acceptor->set_option(gateway_asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec) m_acceptor->bind(endpoint, ec);
    if (!ec) m_acceptor->listen(m_cfg.maxClients > 0 ? m_cfg.maxClients : 1, ec);
    if (ec) {
        closeAllLocked();
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected("listen failed: " + ec.message());
    }

    m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
    startAccept();
    return {};
}

void AsioModbusTcpServer::disconnect() {
    std::lock_guard lk(m_mtx);
    if (m_scheduler) m_scheduler->stopAsync();
    closeAllLocked();
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
}

sched::RequestScheduler& AsioModbusTcpServer::scheduler() {
    return *m_scheduler;
}

transport::ReadResult AsioModbusTcpServer::read(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (req.count <= 0 || req.startAddress < 0) {
        out.errorMessage = "invalid local server read range";
        return out;
    }
    out.values = readLocal(req.table, req.startAddress, req.count);
    if (int(out.values.size()) != req.count) {
        out.errorMessage = "local server read outside table";
        return out;
    }
    out.ok = true;
    return out;
}

transport::WriteResult AsioModbusTcpServer::writeBatch(transport::WriteBatch const& batch) {
    if (batch.startAddress < 0 || batch.values.empty()) {
        return {false, "invalid local server write range"};
    }
    if (!writeLocal(batch.table, batch.startAddress, batch.values, false)) {
        return {false, "local server write outside table"};
    }
    return {true, {}};
}

void AsioModbusTcpServer::startAccept() {
    if (!m_acceptor || state() != transport::ConnectionState::Connected) return;
    auto socket = std::make_shared<TcpSocket>(*m_io);
    m_acceptor->async_accept(*socket, [this, socket](gateway_error_code const& ec) {
        if (state() != transport::ConnectionState::Connected) return;
        if (!ec) {
            std::lock_guard lk(m_mtx);
            m_client = socket;
            startReadHeader(socket);
        }
        startAccept();
    });
}

void AsioModbusTcpServer::startReadHeader(std::shared_ptr<TcpSocket> socket) {
    auto header = std::make_shared<std::array<std::uint8_t, 7>>();
    gateway_asio::async_read(*socket, gateway_asio::buffer(*header),
        [this, socket, header](gateway_error_code const& ec, std::size_t) {
            if (ec || state() != transport::ConnectionState::Connected) return;
            auto const length = getU16(*header, 4);
            if (length < 2 || length > 260) return;
            startReadBody(socket, header, length);
        });
}

void AsioModbusTcpServer::startReadBody(
    std::shared_ptr<TcpSocket> socket,
    std::shared_ptr<std::array<std::uint8_t, 7>> header,
    std::uint16_t length) {
    auto adu = std::make_shared<std::vector<std::uint8_t>>(header->begin(), header->end());
    adu->resize(6 + length);
    gateway_asio::async_read(*socket, gateway_asio::buffer(adu->data() + 7, length - 1),
        [this, socket, adu](gateway_error_code const& ec, std::size_t) {
            if (ec || state() != transport::ConnectionState::Connected) return;
            auto response = std::make_shared<std::vector<std::uint8_t>>(handleRequest(*adu));
            if (response->empty()) {
                startReadHeader(socket);
                return;
            }
            gateway_asio::async_write(*socket, gateway_asio::buffer(*response),
                [this, socket, response](gateway_error_code const& writeEc, std::size_t) {
                    if (!writeEc) startReadHeader(socket);
                });
        });
}

std::vector<std::uint8_t> AsioModbusTcpServer::handleRequest(
    std::vector<std::uint8_t> const& adu) {
    nmbs::ResponseHeader header;
    std::span<std::uint8_t const> pdu;
    std::string error;
    if (!nmbs::parseRequestHeader(adu, header, pdu, error)) return {};
    if (pdu.empty()) return {};

    auto const function = pdu[0];
    if (function == 0x03 || function == 0x04) {
        if (pdu.size() != 5) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        auto const start = getU16(pdu, 1);
        auto const count = getU16(pdu, 3);
        auto values = readLocal(tableForReadFunction(function), start, count);
        if (values.size() != count) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x02);
        }
        return nmbs::buildReadRegistersResponse(
            header.transactionId,
            header.unitId,
            function == 0x03 ? nmbs::Function::ReadHoldingRegisters
                             : nmbs::Function::ReadInputRegisters,
            values);
    }

    if (function == 0x06) {
        if (pdu.size() != 5) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        auto const address = getU16(pdu, 1);
        auto const value = getU16(pdu, 3);
        core::RegisterWords values{value};
        if (!writeLocal(core::RegisterTable::HoldingRegister, address, values, true)) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x02);
        }
        return nmbs::buildWriteSingleRegisterResponse(
            header.transactionId, header.unitId, address, value);
    }

    if (function == 0x10) {
        if (pdu.size() < 6) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        auto const start = getU16(pdu, 1);
        auto const count = getU16(pdu, 3);
        auto const byteCount = pdu[5];
        if (count == 0 || byteCount != count * 2 || pdu.size() != std::size_t(6 + byteCount)) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }

        core::RegisterWords values;
        values.reserve(count);
        for (std::uint16_t i = 0; i < count; i++) {
            values.push_back(getU16(pdu, 6 + i * 2));
        }
        if (!writeLocal(core::RegisterTable::HoldingRegister, start, values, true)) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x02);
        }
        return nmbs::buildWriteMultipleRegistersResponse(
            header.transactionId, header.unitId, start, count);
    }

    return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x01);
}

core::RegisterWords AsioModbusTcpServer::readLocal(core::RegisterTable table, int start, int count) {
    std::lock_guard lk(m_mtx);
    auto it = m_tables.find(table);
    if (it == m_tables.end() || start < 0 || count < 0
        || start + count > int(it->second.size())) {
        return {};
    }
    return core::RegisterWords(it->second.begin() + start, it->second.begin() + start + count);
}

bool AsioModbusTcpServer::writeLocal(core::RegisterTable table,
                                     int start,
                                     core::RegisterWords const& values,
                                     bool publishEvent) {
    {
        std::lock_guard lk(m_mtx);
        ensureRangeLocked(table, start, int(values.size()));
        auto& target = m_tables[table];
        if (start < 0 || start + int(values.size()) > int(target.size())) return false;
        std::copy(values.begin(), values.end(), target.begin() + start);
    }
    if (publishEvent && m_bus) {
        m_bus->publish(bus::ServerWriteEvent{id(), table, start, values});
    }
    return true;
}

void AsioModbusTcpServer::ensureRangeLocked(core::RegisterTable table, int start, int count) {
    if (start < 0 || count < 0) return;
    auto& words = m_tables[table];
    auto const need = std::size_t(start + count);
    if (words.size() < need) words.resize(need, 0);
}

void AsioModbusTcpServer::closeAllLocked() {
    gateway_error_code ignored;
    if (m_client && m_client->is_open()) {
        m_client->shutdown(gateway_asio::ip::tcp::socket::shutdown_both, ignored);
        m_client->close(ignored);
    }
    m_client.reset();
    if (m_acceptor && m_acceptor->is_open()) {
        m_acceptor->cancel(ignored);
        m_acceptor->close(ignored);
    }
    m_acceptor.reset();
}

} // namespace core::gateway
