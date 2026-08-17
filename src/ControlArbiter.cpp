// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/control/ControlArbiter.h"

#include <algorithm>

namespace core::control {

namespace {

bool selectorConflicts(std::string const& lhs, std::string const& rhs) {
    if (lhs.empty() || rhs.empty()) return true;
    if (lhs == rhs) return true;
    auto const prefix = [](std::string const& parent, std::string const& child) {
        return child.size() > parent.size()
            && child.compare(0, parent.size(), parent) == 0
            && child[parent.size()] == '/';
    };
    return prefix(lhs, rhs) || prefix(rhs, lhs);
}

} // namespace

bool ControlAddress::conflictsWith(ControlAddress const& other) const {
    if (protocol != other.protocol || endpoint != other.endpoint
        || resource != other.resource) {
        return false;
    }
    if (protocol == "mqtt") return selectorConflicts(selector, other.selector);
    if (!selector.empty() && !other.selector.empty() && selector != other.selector) {
        return false;
    }
    auto const lhsWidth = std::max<std::int64_t>(1, width);
    auto const rhsWidth = std::max<std::int64_t>(1, other.width);
    if (offset >= other.offset + rhsWidth || other.offset >= offset + lhsWidth) {
        return false;
    }
    if (lhsWidth == 1 && rhsWidth == 1) return (mask & other.mask) != 0;
    return true;
}

bool ControlTarget::conflictsWith(ControlTarget const& other) const {
    if (!id.empty() && id == other.id) return true;
    if (!deviceId.empty() && deviceId != other.deviceId) return false;
    return address.conflictsWith(other.address);
}

void ControlArbiter::setDefaultPolicy(ControlPolicy policy) {
    std::lock_guard lock(m_mtx);
    m_defaultPolicy = std::move(policy);
}

void ControlArbiter::setPolicy(std::string targetId, ControlPolicy policy) {
    std::lock_guard lock(m_mtx);
    m_policies[std::move(targetId)] = std::move(policy);
}

void ControlArbiter::clearPolicies() {
    std::lock_guard lock(m_mtx);
    m_policies.clear();
}

ControlPolicy ControlArbiter::policyFor(std::string const& targetId) const {
    auto const it = m_policies.find(targetId);
    return it == m_policies.end() ? m_defaultPolicy : it->second;
}

ControlDecision ControlArbiter::authorize(ControlRequest const& request,
                                           std::int64_t nowMs) {
    std::lock_guard lock(m_mtx);
    if (request.actor.id.empty()) return {false, false, "actor identity is required"};
    auto const policy = policyFor(request.target.id);
    if (request.actor.priority < policy.minPriority) {
        return {false, false, "actor priority is below policy minimum"};
    }
    if (policy.mode == PolicyMode::ReadOnly) {
        return {false, false, "target is read-only"};
    }
    auto leaseIt = std::find_if(m_leases.begin(), m_leases.end(),
        [&](auto const& item) {
            return item.second.expiresAtMs > nowMs
                && item.second.target.conflictsWith(request.target);
        });
    if (policy.mode == PolicyMode::Open
        || policy.mode == PolicyMode::LastWriterWins
        || policy.mode == PolicyMode::DeviceDecides) {
        if (leaseIt != m_leases.end()
            && leaseIt->second.actorId != request.actor.id) {
            return {false, false, "conflicting target is leased by another actor",
                    leaseIt->second.actorId, leaseIt->second.expiresAtMs};
        }
        return {true, false, "allowed by non-exclusive policy", request.actor.id};
    }

    auto const expired = leaseIt == m_leases.end();
    auto const sameActor = !expired && leaseIt->second.actorId == request.actor.id;
    auto const canPreempt = policy.mode == PolicyMode::PriorityLease
                         && !expired
                         && request.actor.priority > leaseIt->second.priority;
    if (!expired && !sameActor && !canPreempt) {
        return {false, false, "target is leased by another actor",
                leaseIt->second.actorId, leaseIt->second.expiresAtMs};
    }

    auto const preempted = !expired && !sameActor && canPreempt;
    auto const requestedTtl = request.ttlMs > 0 ? request.ttlMs : policy.leaseMs;
    auto const ttl = std::max<std::int64_t>(1, requestedTtl);
    if (leaseIt != m_leases.end() && leaseIt->first != request.target.id) {
        m_leases.erase(leaseIt);
    }
    auto& lease = m_leases[request.target.id];
    lease = {request.target, request.actor.id, request.actor.priority, nowMs + ttl};
    return {true, preempted, preempted ? "higher priority actor preempted lease"
                                      : "lease acquired",
            lease.actorId, lease.expiresAtMs};
}

void ControlArbiter::releaseActor(std::string const& actorId) {
    std::lock_guard lock(m_mtx);
    std::erase_if(m_leases, [&](auto const& item) {
        return item.second.actorId == actorId;
    });
}

void ControlArbiter::releaseTarget(std::string const& targetId) {
    std::lock_guard lock(m_mtx);
    m_leases.erase(targetId);
}

void ControlArbiter::clearLeases() {
    std::lock_guard lock(m_mtx);
    m_leases.clear();
}

std::vector<LeaseSnapshot> ControlArbiter::leases(std::int64_t nowMs) const {
    std::lock_guard lock(m_mtx);
    std::vector<LeaseSnapshot> out;
    for (auto const& [targetId, lease] : m_leases) {
        if (lease.expiresAtMs <= nowMs) continue;
        out.push_back({targetId, lease.actorId, lease.priority, lease.expiresAtMs});
    }
    return out;
}

} // namespace core::control
