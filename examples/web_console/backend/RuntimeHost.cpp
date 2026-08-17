// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "RuntimeHost.h"

#include <chrono>
#include <future>
#include <iostream>
#include <utility>

#include "GatewayAssembly.h"
#include "GatewayJson.h"

#include "core/base/RegisterTable.h"
#include "core/transport/Transport.h"
#include "core/transport/TransportTypes.h"

namespace wc {

namespace {

std::string kindStr(core::transport::TransportKind k) {
    using K = core::transport::TransportKind;
    switch (k) {
        case K::ModbusTcpClient: return "modbus_tcp_client";
        case K::ModbusTcpServer: return "modbus_tcp_server";
        case K::ModbusRtu:       return "modbus_rtu";
        case K::OpcUaClient:     return "opc_ua_client";
        case K::MqttClient:      return "mqtt_client";
        case K::MqttPahoClient:  return "mqtt_client";
        case K::S7Client:        return "s7_client";
    }
    return "unknown";
}

std::string connStr(core::transport::ConnectionState s) {
    using S = core::transport::ConnectionState;
    switch (s) {
        case S::Disconnected: return "disconnected";
        case S::Connecting:   return "connecting";
        case S::Connected:    return "connected";
        case S::Error:        return "error";
    }
    return "unknown";
}

} // namespace

RuntimeHost::RuntimeHost() = default;

RuntimeHost::~RuntimeHost() {
    stop();
}

bool RuntimeHost::start(std::string const& tomlPath) {
    std::lock_guard lifecycleLock(m_lifecycleMtx);
    if (m_running.load()) return true;
    if (m_thread.joinable()) {
        m_workGuard.reset();
        m_io.stop();
        m_thread.join();
    }
    // Drain cancellation completions while the old graph still exists. Reusing
    // an io_context with abandoned handlers would otherwise run callbacks that
    // capture already-destroyed transports after restart.
    if (m_assembly || m_pumpTimer) {
        if (m_assembly) m_assembly->stop();
        m_io.restart();
        while (m_io.poll_one() > 0) {
        }
        m_io.stop();
        m_pumpTimer.reset();
        m_assembly.reset();
    }
    m_io.restart();                              // re-arm io_context for reuse
    m_workGuard.emplace(m_io.get_executor());
    m_assembly = std::make_unique<core::gateway::GatewayAssembly>(m_io);
    try {
        if (!m_assembly->load(tomlPath)) {
            m_workGuard.reset();
            m_assembly.reset();
            return false;
        }
    } catch (std::exception const& exception) {
        std::cerr << "RuntimeHost load failed: " << exception.what() << "\n";
        m_workGuard.reset();
        m_assembly.reset();
        return false;
    } catch (...) {
        std::cerr << "RuntimeHost load failed: unknown error\n";
        m_workGuard.reset();
        m_assembly.reset();
        return false;
    }
    try {
        m_assembly->start();
    } catch (std::exception const& exception) {
        std::cerr << "RuntimeHost start failed: " << exception.what() << "\n";
        m_assembly->stop();
        m_workGuard.reset();
        m_io.restart();
        while (m_io.poll_one() > 0) {
        }
        m_io.stop();
        m_assembly.reset();
        return false;
    } catch (...) {
        std::cerr << "RuntimeHost start failed: unknown error\n";
        m_assembly->stop();
        m_workGuard.reset();
        m_io.restart();
        while (m_io.poll_one() > 0) {
        }
        m_io.stop();
        m_assembly.reset();
        return false;
    }
    m_pumpTimer = std::make_unique<gateway_asio::steady_timer>(m_io);
    m_running.store(true, std::memory_order_release);
    schedulePump();
    m_thread = std::thread([this] {
        while (!m_io.stopped()) {
            try {
                m_io.run();
                break;
            } catch (std::exception const& exception) {
                std::cerr << "RuntimeHost io handler failed: "
                          << exception.what() << "\n";
            } catch (...) {
                std::cerr << "RuntimeHost io handler failed: unknown error\n";
            }
        }
    });
    return true;
}

bool RuntimeHost::validate(std::string const& tomlPath, std::string& error) {
    error.clear();
    gateway_asio::io_context tio;
    try {
        auto tmp = std::make_unique<core::gateway::GatewayAssembly>(tio);
        if (!tmp->load(tomlPath)) {
            error = "config failed to load (see server log)";
            return false;
        }
    } catch (std::exception const& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "config validation failed with an unknown error";
        return false;
    }
    return true;
}

bool RuntimeHost::reload(std::string const& tomlPath) {
    std::lock_guard lifecycleLock(m_lifecycleMtx);
    if (!m_running.load(std::memory_order_acquire) || !m_assembly) {
        return false;
    }

    struct ReloadState {
        std::unique_ptr<core::gateway::GatewayAssembly> candidate;
        std::unique_ptr<core::gateway::GatewayAssembly> retired;
        bool applied = false;
    };
    auto state = std::make_shared<ReloadState>();
    state->candidate =
        std::make_unique<core::gateway::GatewayAssembly>(m_io);
    try {
        if (!state->candidate->load(tomlPath)) return false;
    } catch (std::exception const& exception) {
        std::cerr << "RuntimeHost reload load failed: "
                  << exception.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "RuntimeHost reload load failed: unknown error\n";
        return false;
    }

    bool const dispatched = runOnIoAndWait(
        [this, state] {
            auto previous = std::move(m_assembly);
            previous->stop();
            m_assembly = std::move(state->candidate);
            try {
                m_assembly->start();
                state->applied = true;
                state->retired = std::move(previous);
            } catch (...) {
                auto failed = std::move(m_assembly);
                failed->stop();
                m_assembly = std::move(previous);
                try {
                    m_assembly->start();
                } catch (...) {
                    m_running.store(false, std::memory_order_release);
                }
                state->retired = std::move(failed);
            }
        });
    if (!dispatched) return false;

    // stop() cancels the old graph's asio operations. Run one io barrier before
    // destroying it so their cancellation handlers cannot dereference a
    // transport after its owner has gone away.
    if (!runOnIoAndWait([] {})) return false;
    state->retired.reset();
    if (!state->applied) return false;

    {
        std::lock_guard snapshotLock(m_mtx);
        m_dps.clear();
        m_tps.clear();
    }
    return true;
}

void RuntimeHost::stop() {
    std::lock_guard lifecycleLock(m_lifecycleMtx);
    bool const wasRunning = m_running.exchange(false);
    if (wasRunning) {
        runOnIoAndWait([this] {
            if (m_pumpTimer) {
                try {
                    m_pumpTimer->cancel();
                } catch (...) {
                }
            }
            if (m_assembly) m_assembly->stop();
        });
    } else if (m_assembly) {
        m_assembly->stop();
    }
    m_workGuard.reset();
    m_io.stop();
    if (m_thread.joinable()) m_thread.join();
    m_io.restart();
    while (m_io.poll_one() > 0) {
    }
    m_io.stop();
    m_pumpTimer.reset();
    m_assembly.reset();
    {
        std::lock_guard snapshotLock(m_mtx);
        m_dps.clear();
        m_tps.clear();
    }
}

bool RuntimeHost::runOnIoAndWait(std::function<void()> operation) {
    if (!operation) return true;
    if (m_thread.joinable()
        && std::this_thread::get_id() == m_thread.get_id()) {
        operation();
        return true;
    }
    if (!m_thread.joinable() || m_io.stopped()) return false;

    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();
    try {
        gateway_asio::post(
            m_io,
            [operation = std::move(operation), completed]() mutable {
                try {
                    operation();
                    completed->set_value();
                } catch (...) {
                    completed->set_exception(std::current_exception());
                }
            });
    } catch (...) {
        return false;
    }
    try {
        future.get();
        return true;
    } catch (...) {
        return false;
    }
}

void RuntimeHost::schedulePump() {
    if (!m_pumpTimer) return;
    m_pumpTimer->expires_after(std::chrono::milliseconds(500));
    m_pumpTimer->async_wait([this](gateway_error_code const& ec) {
        if (ec || !m_running.load(std::memory_order_acquire) || !m_assembly) return;
        std::vector<core::gateway::GatewayDatapointSnapshot> dps;
        std::vector<core::gateway::GatewayTransportSnapshot> tps;
        std::vector<core::gateway::DriverSnapshot> drivers;
        std::vector<core::control::DeviceRoute> routes;
        std::vector<core::control::LeaseSnapshot> leases;
        std::vector<core::gateway::GatewayDriverDataSnapshot> driverData;
        try {
            dps = m_assembly->datapointSnapshots();
            tps = m_assembly->transportSnapshots();
            drivers = m_assembly->driverSnapshots();
            routes = m_assembly->deviceRoutes();
            leases = m_assembly->controlLeases();
            driverData = m_assembly->driverDataSnapshots();
        } catch (std::exception const& exception) {
            std::cerr << "RuntimeHost snapshot failed: "
                      << exception.what() << "\n";
            schedulePump();
            return;
        } catch (...) {
            std::cerr << "RuntimeHost snapshot failed: unknown error\n";
            schedulePump();
            return;
        }
        std::vector<DpSnap> ds;
        ds.reserve(dps.size());
        for (auto const& d : dps) {
            ds.push_back({d.id, d.value, core::gateway::json::dpState(d.state),
                          core::gateway::json::timestampMs(d.timestamp)});
        }
        std::vector<TpSnap> ts;
        ts.reserve(tps.size());
        for (auto const& t : tps) ts.push_back({t.id, kindStr(t.kind), connStr(t.state)});
        std::vector<DriverSnap> driverSnaps;
        for (auto const& d : drivers) {
            driverSnaps.push_back({d.id, d.library, d.state, d.error});
        }
        std::vector<RouteSnap> routeSnaps;
        for (auto const& r : routes) {
            routeSnaps.push_back({r.id, r.deviceId, r.driverId, r.transportId,
                                  r.protocol, r.writable, r.active});
        }
        std::vector<LeaseSnap> leaseSnaps;
        for (auto const& l : leases) {
            leaseSnaps.push_back(
                {l.targetId, l.actorId, l.priority, l.expiresAtMs});
        }
        std::vector<DriverDataSnap> dataSnaps;
        for (auto& d : driverData) {
            dataSnaps.push_back({std::move(d.driverId), std::move(d.deviceId),
                                 std::move(d.targetId), std::move(d.payload),
                                 d.timestampMs});
        }
        {
            std::lock_guard lk(m_mtx);
            m_dps = std::move(ds);
            m_tps = std::move(ts);
            m_drivers = std::move(driverSnaps);
            m_routes = std::move(routeSnaps);
            m_leases = std::move(leaseSnaps);
            m_driverData = std::move(dataSnaps);
        }
        schedulePump();
    });
}

std::vector<DpSnap> RuntimeHost::datapoints() const {
    std::lock_guard lk(m_mtx);
    return m_dps;
}

std::vector<TpSnap> RuntimeHost::transports() const {
    std::lock_guard lk(m_mtx);
    return m_tps;
}

std::vector<DriverSnap> RuntimeHost::drivers() const {
    std::lock_guard lk(m_mtx);
    return m_drivers;
}

std::vector<RouteSnap> RuntimeHost::routes() const {
    std::lock_guard lk(m_mtx);
    return m_routes;
}

std::vector<LeaseSnap> RuntimeHost::leases() const {
    std::lock_guard lk(m_mtx);
    return m_leases;
}

std::vector<DriverDataSnap> RuntimeHost::driverData() const {
    std::lock_guard lk(m_mtx);
    return m_driverData;
}

bool RuntimeHost::write(std::string const& transportId, int startAddress,
                        std::vector<std::uint16_t> values,
                        std::function<void(bool, std::string)> done) {
    if (!done) return false;
    if (transportId.empty() || startAddress < 0 || startAddress > 65535
        || values.empty() || values.size() > 123
        || std::int64_t(startAddress) + std::int64_t(values.size()) > 65536) {
        done(false, "invalid write range");
        return false;
    }
    if (!m_running.load()) { done(false, "runtime not running"); return false; }
    auto completion = std::make_shared<
        std::function<void(bool, std::string)>>(std::move(done));
    try {
        gateway_asio::post(
            m_io,
            [this, transportId, startAddress, values = std::move(values),
             completion]() mutable {
                if (!m_running.load(std::memory_order_acquire)
                    || !m_assembly) {
                    (*completion)(false, "runtime stopped");
                    return;
                }
                try {
                    core::control::ActorContext actor;
                    actor.id = "conversion-engine";
                    actor.clientId = "conversion-engine";
                    actor.channel = "internal";
                    bool known = m_assembly->writeRegisterControlAsync(
                        std::move(actor),
                        transportId,
                        startAddress,
                        core::RegisterWords(values.begin(), values.end()),
                        [completion](bool ok, std::string error) {
                            (*completion)(ok, std::move(error));
                        });
                    if (!known) {
                        (*completion)(false, "unknown control target");
                    }
                } catch (std::exception const& exception) {
                    (*completion)(false, exception.what());
                } catch (...) {
                    (*completion)(
                        false,
                        "write failed with an unknown error");
                }
            });
    } catch (std::exception const& exception) {
        (*completion)(false, exception.what());
        return false;
    } catch (...) {
        (*completion)(false, "failed to schedule runtime write");
        return false;
    }
    return true;
}

bool RuntimeHost::writeControl(
    std::string actorId, std::string targetId,
    std::vector<std::uint8_t> payload,
    std::function<void(bool, std::string)> done) {
    if (!done || actorId.empty() || targetId.empty() || payload.empty()) {
        if (done) done(false, "actor, target and payload are required");
        return false;
    }
    if (!m_running.load()) {
        done(false, "runtime not running");
        return false;
    }
    auto completion = std::make_shared<
        std::function<void(bool, std::string)>>(std::move(done));
    gateway_asio::post(m_io,
        [this, actorId = std::move(actorId), targetId = std::move(targetId),
         payload = std::move(payload), completion]() mutable {
            if (!m_assembly) {
                (*completion)(false, "runtime stopped");
                return;
            }
            core::control::ActorContext actor;
            actor.id = actorId;
            actor.clientId = actorId;
            actor.channel = "web";
            if (!m_assembly->writeControlAsync(
                    std::move(actor), targetId, std::move(payload),
                    [completion](bool ok, std::string error) {
                        (*completion)(ok, std::move(error));
                    })) {
                (*completion)(false, "unknown control target");
            }
        });
    return true;
}

bool RuntimeHost::activateRoute(
    std::string deviceId, std::string routeId,
    std::function<void(bool, std::string)> done) {
    if (!done || deviceId.empty() || routeId.empty()) return false;
    if (!m_running.load()) {
        done(false, "runtime not running");
        return false;
    }
    auto completion = std::make_shared<
        std::function<void(bool, std::string)>>(std::move(done));
    gateway_asio::post(m_io,
        [this, deviceId = std::move(deviceId), routeId = std::move(routeId),
         completion]() {
            std::string error;
            auto const ok = m_assembly
                && m_assembly->setActiveDeviceRoute(deviceId, routeId, error);
            (*completion)(ok, std::move(error));
        });
    return true;
}

} // namespace wc
