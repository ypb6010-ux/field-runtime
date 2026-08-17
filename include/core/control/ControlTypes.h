// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

namespace core::control {

enum class PolicyMode {
    Open,
    ExclusiveLease,
    PriorityLease,
    LastWriterWins,
    DeviceDecides,
    ReadOnly,
};

struct ActorContext {
    std::string id;
    std::string clientId;
    std::string sessionId;
    std::string sourceAddress;
    std::string channel;
    std::string role;
    int priority = 0;
};

struct ControlAddress {
    std::string protocol;
    std::string endpoint;
    std::string resource;
    std::string selector;
    std::int64_t offset = 0;
    std::int64_t width = 1;
    std::uint64_t mask = ~std::uint64_t{0};

    bool conflictsWith(ControlAddress const& other) const;
};

struct ControlTarget {
    std::string id;
    std::string deviceId;
    std::string routeId;
    ControlAddress address;

    bool conflictsWith(ControlTarget const& other) const;
};

struct ControlPolicy {
    std::string id;
    PolicyMode mode = PolicyMode::Open;
    std::int64_t leaseMs = 0;
    int minPriority = 0;
};

struct ControlRequest {
    std::string requestId;
    ActorContext actor;
    ControlTarget target;
    std::int64_t timestampMs = 0;
    std::int64_t ttlMs = 0;
};

struct ControlDecision {
    bool allowed = false;
    bool preempted = false;
    std::string reason;
    std::string ownerId;
    std::int64_t leaseExpiresAtMs = 0;
};

struct LeaseSnapshot {
    std::string targetId;
    std::string actorId;
    int priority = 0;
    std::int64_t expiresAtMs = 0;
};

} // namespace core::control
