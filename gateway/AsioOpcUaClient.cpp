// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "AsioOpcUaClient.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/types.h>

namespace core::gateway {

namespace {

// Reduce an OPC UA scalar to a single register word so the value flows through
// the same datapoint/codec pipeline as a Modbus register (scale / enum / etc.).
bool variantToU16(UA_Variant const& v, std::uint16_t& out) {
    if (!UA_Variant_isScalar(&v) || v.data == nullptr || v.type == nullptr) {
        return false;
    }
    auto const* t = v.type;
    if (t == &UA_TYPES[UA_TYPES_BOOLEAN]) { out = *static_cast<UA_Boolean*>(v.data) ? 1 : 0; return true; }
    if (t == &UA_TYPES[UA_TYPES_SBYTE])   { out = std::uint16_t(std::int16_t(*static_cast<UA_SByte*>(v.data)));  return true; }
    if (t == &UA_TYPES[UA_TYPES_BYTE])    { out = *static_cast<UA_Byte*>(v.data);  return true; }
    if (t == &UA_TYPES[UA_TYPES_INT16])   { out = std::uint16_t(*static_cast<UA_Int16*>(v.data));  return true; }
    if (t == &UA_TYPES[UA_TYPES_UINT16])  { out = *static_cast<UA_UInt16*>(v.data); return true; }
    if (t == &UA_TYPES[UA_TYPES_INT32])   { out = std::uint16_t(*static_cast<UA_Int32*>(v.data));  return true; }
    if (t == &UA_TYPES[UA_TYPES_UINT32])  { out = std::uint16_t(*static_cast<UA_UInt32*>(v.data)); return true; }
    if (t == &UA_TYPES[UA_TYPES_INT64])   { out = std::uint16_t(*static_cast<UA_Int64*>(v.data));  return true; }
    if (t == &UA_TYPES[UA_TYPES_UINT64])  { out = std::uint16_t(*static_cast<UA_UInt64*>(v.data)); return true; }
    if (t == &UA_TYPES[UA_TYPES_FLOAT])   { out = std::uint16_t(std::int64_t(*static_cast<UA_Float*>(v.data)));  return true; }
    if (t == &UA_TYPES[UA_TYPES_DOUBLE])  { out = std::uint16_t(std::int64_t(*static_cast<UA_Double*>(v.data))); return true; }
    return false;
}

// Parse "ns=<n>;s=<str>" / "ns=<n>;i=<num>" / "s=<str>" / "i=<num>" into a
// UA_NodeId. Caller owns the result and must UA_NodeId_clear() it.
bool parseNodeId(std::string const& spec, UA_NodeId& out) {
    UA_UInt16 ns = 0;
    std::string rest = spec;
    if (rest.rfind("ns=", 0) == 0) {
        auto const semi = rest.find(';');
        if (semi == std::string::npos) return false;
        try {
            ns = UA_UInt16(std::stoul(rest.substr(3, semi - 3)));
        } catch (...) {
            return false;
        }
        rest = rest.substr(semi + 1);
    }
    if (rest.rfind("i=", 0) == 0) {
        try {
            out = UA_NODEID_NUMERIC(ns, UA_UInt32(std::stoul(rest.substr(2))));
            return true;
        } catch (...) {
            return false;
        }
    }
    if (rest.rfind("s=", 0) == 0) {
        out = UA_NODEID_STRING_ALLOC(ns, rest.substr(2).c_str());
        return true;
    }
    return false;
}

} // namespace

AsioOpcUaClient::AsioOpcUaClient(config::TransportConfig cfg,
                                 gateway_asio::io_context& mainIo)
    : m_cfg(std::move(cfg))
    , m_mainIo(&mainIo)
    , m_scheduler(sched::makeScheduler(m_cfg.scheduler))
    , m_workGuard(gateway_asio::make_work_guard(m_workerIo)) {
    // Scheduler timing runs on the main io thread (where submitAsync is driven),
    // matching the Modbus client.
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_mainIo);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });
    m_workerThread = std::thread([this] { m_workerIo.run(); });
}

AsioOpcUaClient::~AsioOpcUaClient() {
    if (m_scheduler) m_scheduler->stopAsync();
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    gateway_asio::post(m_workerIo, [this, done] {
        if (m_client) {
            UA_Client_disconnect(m_client);
            UA_Client_delete(m_client);
            m_client = nullptr;
        }
        done->set_value();
    });
    fut.wait();
    m_workGuard.reset();
    m_workerIo.stop();
    if (m_workerThread.joinable()) m_workerThread.join();
}

std::string AsioOpcUaClient::id() const {
    return m_cfg.id;
}

transport::TransportKind AsioOpcUaClient::kind() const {
    return transport::TransportKind::OpcUaClient;
}

transport::ConnectionState AsioOpcUaClient::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> AsioOpcUaClient::connect() {
    if (m_scheduler) m_scheduler->startAsync();
    m_state.store(transport::ConnectionState::Connecting, std::memory_order_release);

    auto result = std::make_shared<std::promise<std::expected<void, std::string>>>();
    auto fut = result->get_future();
    gateway_asio::post(m_workerIo, [this, result] {
        if (!m_client) {
            m_client = UA_Client_new();
            UA_ClientConfig_setDefault(UA_Client_getConfig(m_client));
        }
        UA_StatusCode rc = UA_Client_connect(m_client, m_cfg.endpointUrl.c_str());
        if (rc != UA_STATUSCODE_GOOD) {
            UA_Client_delete(m_client);
            m_client = nullptr;
            m_state.store(transport::ConnectionState::Error, std::memory_order_release);
            result->set_value(std::unexpected(
                std::string("OPC UA connect '") + m_cfg.endpointUrl + "' failed: "
                + UA_StatusCode_name(rc)));
            return;
        }
        m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
        result->set_value({});
    });

    auto const timeoutMs = m_cfg.connectTimeoutMs > 0 ? m_cfg.connectTimeoutMs : 3000;
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        m_state.store(transport::ConnectionState::Error, std::memory_order_release);
        return std::unexpected(std::string("OPC UA connect timeout: ") + m_cfg.endpointUrl);
    }
    return fut.get();
}

void AsioOpcUaClient::disconnect() {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    gateway_asio::post(m_workerIo, [this, done] {
        if (m_client) {
            UA_Client_disconnect(m_client);
            UA_Client_delete(m_client);
            m_client = nullptr;
        }
        done->set_value();
    });
    fut.wait();
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
}

sched::RequestScheduler& AsioOpcUaClient::scheduler() {
    return *m_scheduler;
}

std::string AsioOpcUaClient::nodeIdFor(int address) const {
    std::string out = m_cfg.nodeIdTemplate;
    auto const pos = out.find("%1");
    if (pos != std::string::npos) out.replace(pos, 2, std::to_string(address));
    return out;
}

transport::ReadResult AsioOpcUaClient::readOnWorker(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (!m_client || state() != transport::ConnectionState::Connected) {
        out.errorMessage = "OPC UA client is not connected";
        return out;
    }

    core::RegisterWords words(req.count > 0 ? std::size_t(req.count) : 0, 0);
    for (int i = 0; i < req.count; i++) {
        std::string const spec = nodeIdFor(req.startAddress + i);
        UA_NodeId nodeId;
        if (!parseNodeId(spec, nodeId)) {
            out.errorMessage = "OPC UA bad node id '" + spec + "'";
            return out;
        }
        UA_Variant value;
        UA_Variant_init(&value);
        UA_StatusCode rc = UA_Client_readValueAttribute(m_client, nodeId, &value);
        UA_NodeId_clear(&nodeId);
        if (rc != UA_STATUSCODE_GOOD) {
            UA_Variant_clear(&value);
            out.errorMessage = std::string("OPC UA read failed @ ") + spec + ": "
                + UA_StatusCode_name(rc);
            if (rc == UA_STATUSCODE_BADCONNECTIONCLOSED
                || rc == UA_STATUSCODE_BADSERVERNOTCONNECTED
                || rc == UA_STATUSCODE_BADCONNECTIONREJECTED) {
                m_state.store(transport::ConnectionState::Error, std::memory_order_release);
            }
            return out;
        }
        std::uint16_t word = 0;
        bool const ok = variantToU16(value, word);
        UA_Variant_clear(&value);
        if (!ok) {
            out.errorMessage = "OPC UA unsupported value type @ " + spec;
            return out;
        }
        words[std::size_t(i)] = word;
    }

    out.ok = true;
    out.values = std::move(words);
    return out;
}

transport::ReadResult AsioOpcUaClient::read(transport::ReadRequest const& req) {
    auto result = std::make_shared<std::promise<transport::ReadResult>>();
    auto fut = result->get_future();
    gateway_asio::post(m_workerIo, [this, req, result] {
        result->set_value(readOnWorker(req));
    });
    return fut.get();
}

void AsioOpcUaClient::readAsync(transport::ReadRequest const& req, ReadDone done) {
    if (state() != transport::ConnectionState::Connected) {
        transport::ReadResult out;
        out.startAddress = req.startAddress;
        out.errorMessage = "OPC UA client is not connected";
        done(std::move(out));
        return;
    }
    auto* mainIo = m_mainIo;
    gateway_asio::post(m_workerIo,
        [this, req, mainIo, done = std::move(done)]() mutable {
            auto result = readOnWorker(req);
            gateway_asio::post(*mainIo,
                [done = std::move(done), result = std::move(result)]() mutable {
                    done(std::move(result));
                });
        });
}

// Writes are not implemented for this card (read-only southbound).
transport::WriteResult AsioOpcUaClient::writeBatch(transport::WriteBatch const&) {
    return {false, "OPC UA write not supported"};
}

void AsioOpcUaClient::writeAsync(transport::WriteBatch const&, WriteDone done) {
    done({false, "OPC UA write not supported"});
}

} // namespace core::gateway
