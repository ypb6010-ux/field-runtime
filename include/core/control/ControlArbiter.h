// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/control/ControlTypes.h"
#include "core/core_global.h"

namespace core::control {

class CORE_EXPORT ControlArbiter {
public:
    void setDefaultPolicy(ControlPolicy policy);
    void setPolicy(std::string targetId, ControlPolicy policy);
    void clearPolicies();

    ControlDecision authorize(ControlRequest const& request,
                              std::int64_t nowMs);
    void releaseActor(std::string const& actorId);
    void releaseTarget(std::string const& targetId);
    void clearLeases();
    std::vector<LeaseSnapshot> leases(std::int64_t nowMs) const;

private:
    struct Lease {
        ControlTarget target;
        std::string actorId;
        int priority = 0;
        std::int64_t expiresAtMs = 0;
    };

    ControlPolicy policyFor(std::string const& targetId) const;

    mutable std::mutex m_mtx;
    ControlPolicy m_defaultPolicy;
    std::map<std::string, ControlPolicy> m_policies;
    std::map<std::string, Lease> m_leases;
};

} // namespace core::control
