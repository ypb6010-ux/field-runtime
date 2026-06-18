// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// field_console_dataservice — hardware-free data simulation for the web_console
// example. Exposes three southbound faces driven by one SimEngine so the gateway
// / console see "moving" data without real devices:
//   - Modbus TCP server  (default :1502, unit 1, HR)
//   - S7 server          (snap7, :102, DB1)
//   - OPC UA server      (open62541, opc.tcp://:4840, ns=2;s=Sim_<addr>)
// See docs/DATA_SERVICE.md for the data dictionary. MQTT face: TODO (needs broker).
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "SimEngine.h"

#include "GatewayAsio.h"
#include "nanomodbus.h"

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/types.h>

#include <snap7/snap7_libmain.h>

namespace {

std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

double nowSeconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::uint16_t getU16(std::span<std::uint8_t const> d, std::size_t pos) {
    return std::uint16_t((std::uint16_t(d[pos]) << 8) | d[pos + 1]);
}

// ── Modbus face: blocking accept/serve loop on its own thread, holding[] guarded
//    by a mutex shared with the SimEngine ticker. ──────────────────────────────
struct ModbusFace {
    std::vector<std::uint16_t> holding = std::vector<std::uint16_t>(128, 0);
    std::mutex mtx;
};

std::vector<std::uint8_t> serveModbus(ModbusFace& face, std::span<std::uint8_t const> adu) {
    nmbs::ResponseHeader h;
    std::span<std::uint8_t const> pdu;
    std::string err;
    if (!nmbs::parseRequestHeader(adu, h, pdu, err) || pdu.empty()) return {};
    auto const fn = pdu[0];
    std::lock_guard lk(face.mtx);
    if (fn == 0x03 || fn == 0x04) {
        if (pdu.size() != 5) return nmbs::buildExceptionResponse(h.transactionId, h.unitId, fn, 0x03);
        auto const start = getU16(pdu, 1), count = getU16(pdu, 3);
        if (count == 0 || std::size_t(start) + count > face.holding.size())
            return nmbs::buildExceptionResponse(h.transactionId, h.unitId, fn, 0x02);
        return nmbs::buildReadRegistersResponse(
            h.transactionId, h.unitId,
            fn == 0x03 ? nmbs::Function::ReadHoldingRegisters : nmbs::Function::ReadInputRegisters,
            std::span<std::uint16_t const>(face.holding.data() + start, count));
    }
    if (fn == 0x10) {
        if (pdu.size() < 6) return nmbs::buildExceptionResponse(h.transactionId, h.unitId, fn, 0x03);
        auto const start = getU16(pdu, 1), count = getU16(pdu, 3);
        if (count == 0 || std::size_t(start) + count > face.holding.size())
            return nmbs::buildExceptionResponse(h.transactionId, h.unitId, fn, 0x02);
        for (std::uint16_t i = 0; i < count; i++) face.holding[start + i] = getU16(pdu, 6 + i * 2);
        return nmbs::buildWriteMultipleRegistersResponse(h.transactionId, h.unitId, start, count);
    }
    return nmbs::buildExceptionResponse(h.transactionId, h.unitId, fn, 0x01);
}

void runModbusFace(ModbusFace& face, unsigned short port) {
    gateway_asio::io_context io;
    gateway_asio::ip::tcp::acceptor acceptor(
        io, gateway_asio::ip::tcp::endpoint(gateway_asio::ip::address_v4::any(), port));
    acceptor.non_blocking(true);
    while (g_running.load()) {
        gateway_asio::ip::tcp::socket socket(io);
        gateway_error_code ec;
        acceptor.accept(socket, ec);
        if (ec == gateway_asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (ec) continue;
        socket.non_blocking(false);
        for (;;) {
            std::array<std::uint8_t, 7> hdr{};
            gateway_asio::read(socket, gateway_asio::buffer(hdr), ec);
            if (ec) break;
            auto const len = getU16(hdr, 4);
            if (len < 2 || len > 260) break;
            std::vector<std::uint8_t> adu(hdr.begin(), hdr.end());
            adu.resize(6 + len);
            gateway_asio::read(socket, gateway_asio::buffer(adu.data() + 7, len - 1), ec);
            if (ec) break;
            auto resp = serveModbus(face, adu);
            if (resp.empty()) break;
            gateway_asio::write(socket, gateway_asio::buffer(resp), ec);
            if (ec) break;
        }
    }
}

// ── OPC UA face: open62541 server on its own thread; a repeated callback (server
//    thread) evaluates the OPC UA points and writes the typed nodes. ───────────
struct OpcFace {
    sim::SimEngine* engine = nullptr;
    std::vector<std::size_t> idx;      // indices of OPC points in engine
    std::chrono::steady_clock::time_point start;
    UA_UInt16 ns = 2;
};

void addOpcNode(UA_Server* server, UA_UInt16 ns, char const* name, UA_NodeId const* typeId) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.dataType = *typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));
    UA_Server_addVariableNode(
        server, UA_NODEID_STRING(ns, const_cast<char*>(name)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(ns, const_cast<char*>(name)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, nullptr, nullptr);
}

void opcTick(UA_Server* server, void* data) {
    auto* f = static_cast<OpcFace*>(data);
    double const t = nowSeconds(f->start);
    for (auto i : f->idx) {
        auto& p = f->engine->points()[i];
        double v = sim::SimEngine::eval(p, t);
        std::string node = "Sim_" + std::to_string(p.addr);
        UA_NodeId id = UA_NODEID_STRING(f->ns, const_cast<char*>(node.c_str()));
        UA_Variant var; UA_Variant_init(&var);
        if (p.type == sim::Type::Bool) {
            UA_Boolean b = v >= 0.5; UA_Variant_setScalar(&var, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
            UA_Server_writeValue(server, id, var);
        } else if (p.type == sim::Type::F32) {
            UA_Float fl = float(v); UA_Variant_setScalar(&var, &fl, &UA_TYPES[UA_TYPES_FLOAT]);
            UA_Server_writeValue(server, id, var);
        } else {
            UA_Int32 n = UA_Int32(std::llround(v)); UA_Variant_setScalar(&var, &n, &UA_TYPES[UA_TYPES_INT32]);
            UA_Server_writeValue(server, id, var);
        }
    }
}

void runOpcFace(OpcFace& f, std::uint16_t port) {
    UA_Server* server = UA_Server_new();
    UA_ServerConfig_setMinimal(UA_Server_getConfig(server), port, nullptr);
    f.ns = UA_Server_addNamespace(server, "http://fieldruntime/sim");
    for (auto i : f.idx) {
        auto const& p = f.engine->points()[i];
        std::string node = "Sim_" + std::to_string(p.addr);
        UA_NodeId const* typeId = (p.type == sim::Type::Bool) ? &UA_TYPES[UA_TYPES_BOOLEAN].typeId
                                : (p.type == sim::Type::F32)  ? &UA_TYPES[UA_TYPES_FLOAT].typeId
                                                              : &UA_TYPES[UA_TYPES_INT32].typeId;
        addOpcNode(server, f.ns, node.c_str(), typeId);
    }
    UA_UInt64 cbId = 0;
    UA_Server_addRepeatedCallback(server, opcTick, &f, 200.0, &cbId);
    std::cout << "  OPC UA face on opc.tcp://0.0.0.0:" << port << " (ns=" << f.ns << ")\n";
    // Iterate so we can honor g_running (atomic<bool> can't be passed to
    // UA_Server_run's volatile UA_Boolean*). The repeated callback still fires.
    UA_Server_run_startup(server);
    while (g_running.load()) UA_Server_run_iterate(server, true);
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
}

} // namespace

int main(int argc, char** argv) {
    unsigned short const modbusPort = argc > 1 ? static_cast<unsigned short>(std::stoi(argv[1])) : 1502;
    char const* s7Addr = argc > 2 ? argv[2] : "0.0.0.0";
    std::uint16_t const opcPort = argc > 3 ? static_cast<std::uint16_t>(std::stoi(argv[3])) : 4840;
    int const s7Db = 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto engine = sim::SimEngine::defaultCatalog();
    auto const start = std::chrono::steady_clock::now();

    // Partition point indices by face.
    std::vector<std::size_t> modbusIdx, s7Idx, opcIdx;
    for (std::size_t i = 0; i < engine.points().size(); i++) {
        switch (engine.points()[i].face) {
            case sim::Face::Modbus: modbusIdx.push_back(i); break;
            case sim::Face::S7:     s7Idx.push_back(i); break;
            case sim::Face::OpcUa:  opcIdx.push_back(i); break;
        }
    }

    // Modbus face thread.
    ModbusFace modbus;
    std::thread modbusThread([&] { runModbusFace(modbus, modbusPort); });

    // S7 face (snap7 manages its own thread; DB buffer updated by the ticker).
    std::vector<std::uint8_t> dbBuf(1024, 0);
    S7Object s7 = Srv_Create();
    Srv_RegisterArea(s7, srvAreaDB, word(s7Db), dbBuf.data(), int(dbBuf.size()));
    int s7rc = Srv_StartTo(s7, s7Addr);

    // OPC UA face thread.
    OpcFace opc; opc.engine = &engine; opc.idx = opcIdx; opc.start = start;
    std::thread opcThread([&] { runOpcFace(opc, opcPort); });

    std::cout << "field_console_dataservice up:\n"
              << "  Modbus face on 0.0.0.0:" << modbusPort << " (unit 1, HR)\n"
              << "  S7 face on " << s7Addr << ":102 DB" << s7Db
              << (s7rc == 0 ? "" : " [Srv_StartTo failed]") << "\n";

    // SimEngine ticker (main thread): update Modbus + S7 faces every 200 ms.
    while (g_running.load()) {
        double const t = nowSeconds(start);
        {
            std::lock_guard lk(modbus.mtx);
            for (auto i : modbusIdx) {
                auto& p = engine.points()[i];
                auto words = sim::SimEngine::encode(p, sim::SimEngine::eval(p, t));
                for (int w = 0; w < int(words.size()); w++)
                    if (std::size_t(p.addr + w) < modbus.holding.size())
                        modbus.holding[p.addr + w] = words[std::size_t(w)];
            }
        }
        for (auto i : s7Idx) {
            auto& p = engine.points()[i];
            auto words = sim::SimEngine::encode(p, sim::SimEngine::eval(p, t));
            for (int w = 0; w < int(words.size()); w++) {
                std::size_t byte = std::size_t(p.addr + w) * 2;       // word idx -> byte
                if (byte + 1 < dbBuf.size()) {
                    dbBuf[byte]     = std::uint8_t(words[std::size_t(w)] >> 8);
                    dbBuf[byte + 1] = std::uint8_t(words[std::size_t(w)] & 0xFFu);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    Srv_Stop(s7);
    Srv_Destroy(s7);
    if (modbusThread.joinable()) modbusThread.join();
    if (opcThread.joinable()) opcThread.join();
    return EXIT_SUCCESS;
}
