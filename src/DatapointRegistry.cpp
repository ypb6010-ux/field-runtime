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

std::shared_ptr<Datapoint> DatapointRegistry::find(QString const& id) const {
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return nullptr;
    return it->second;
}

QList<std::shared_ptr<Datapoint>> DatapointRegistry::all() const {
    QList<std::shared_ptr<Datapoint>> out;
    out.reserve(int(m_byId.size()));
    for (auto const& [_, dp] : m_byId) out.append(dp);
    return out;
}

} // namespace core::dp
