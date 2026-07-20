// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/dp/DatapointRegistry.h"

#include <mutex>
#include <utility>

#include "core/dp/Datapoint.h"

namespace core::dp {

DatapointRegistry::DatapointRegistry()  = default;
DatapointRegistry::~DatapointRegistry() = default;

bool DatapointRegistry::registerDp(std::shared_ptr<Datapoint> dp) {
    if (!dp) return false;
    QString const id = dp->id();
    if (id.trimmed().isEmpty()) return false;
    std::unique_lock lk(m_mutex);
    return m_byId.emplace(id, std::move(dp)).second;
}

std::shared_ptr<Datapoint> DatapointRegistry::find(QString const& id) const {
    std::shared_lock lk(m_mutex);
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return nullptr;
    return it->second;
}

QList<std::shared_ptr<Datapoint>> DatapointRegistry::all() const {
    std::shared_lock lk(m_mutex);
    QList<std::shared_ptr<Datapoint>> out;
    out.reserve(int(m_byId.size()));
    for (auto const& [_, dp] : m_byId) out.append(dp);
    return out;
}

} // namespace core::dp
