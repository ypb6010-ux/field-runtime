// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "GatewayAsio.h"

#include "core/dp/Value.h"

namespace core::gateway { class GatewayAssembly; }

namespace wc {

struct DpSnap {
    std::string id;
    core::dp::Value value;
    std::string state;
    std::int64_t ts = 0;
};
struct TpSnap {
    std::string id;
    std::string kind;
    std::string state;
};

// Hosts a FieldRuntime runtime (gateway GatewayAssembly) in-process on its own
// asio io thread, and keeps a thread-safe copy of the latest datapoint /
// transport state for the Drogon controllers to read (the trantor<->asio
// bridge: single-writer on the io thread, many readers on Drogon threads).
class RuntimeHost {
public:
    RuntimeHost();
    ~RuntimeHost();

    // Build + start the runtime from a gateway TOML. Returns false if load fails.
    bool start(std::string const& tomlPath);
    void stop();
    bool running() const { return m_running.load(std::memory_order_acquire); }

    // Validate a config (load into a throwaway assembly, no start). Static so it
    // never touches the running runtime.
    static bool validate(std::string const& tomlPath, std::string& error);
    // Transactional hot reload: build the candidate without touching the live
    // graph, then swap on the io thread. A failed candidate leaves the current
    // runtime running; a start exception rolls back to the previous graph.
    bool reload(std::string const& tomlPath);

    std::vector<DpSnap> datapoints() const;
    std::vector<TpSnap> transports() const;

    // Control write to a transport (HoldingRegisters). done() runs on the io
    // thread with {ok,error}.
    bool write(std::string const& transportId, int startAddress,
               std::vector<std::uint16_t> values,
               std::function<void(bool, std::string)> done);

private:
    void schedulePump();
    bool runOnIoAndWait(std::function<void()> operation);

    gateway_asio::io_context m_io;
    std::optional<gateway_asio::executor_work_guard<gateway_asio::io_context::executor_type>> m_workGuard;
    std::unique_ptr<core::gateway::GatewayAssembly> m_assembly;
    std::unique_ptr<gateway_asio::steady_timer> m_pumpTimer;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_lifecycleMtx;

    mutable std::mutex m_mtx;
    std::vector<DpSnap> m_dps;
    std::vector<TpSnap> m_tps;
};

} // namespace wc
