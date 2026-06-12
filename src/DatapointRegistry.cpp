// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/dp/DatapointRegistry.h"

#include <utility>

#include "core/dp/Datapoint.h"

namespace core::dp {

DatapointRegistry::DatapointRegistry()  = default;
DatapointRegistry::~DatapointRegistry() = default;

void DatapointRegistry::registerDp(std::shared_ptr<Datapoint> dp) {
    if (!dp) return;
    m_byId.insert_or_assign(dp->id(), std::move(dp));
}

std::shared_ptr<Datapoint> DatapointRegistry::find(std::string const& id) const {
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return nullptr;
    return it->second;
}

std::vector<std::shared_ptr<Datapoint>> DatapointRegistry::all() const {
    std::vector<std::shared_ptr<Datapoint>> out;
    out.reserve(m_byId.size());
    for (auto const& [_, dp] : m_byId) out.push_back(dp);
    return out;
}

} // namespace core::dp
