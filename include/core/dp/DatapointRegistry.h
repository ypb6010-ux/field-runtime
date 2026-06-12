// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/core_global.h"

namespace core::dp {

class Datapoint;

class CORE_EXPORT DatapointRegistry {
public:
    DatapointRegistry();
    ~DatapointRegistry();

    CORE_DISABLE_COPY_MOVE(DatapointRegistry)

    void                       registerDp(std::shared_ptr<Datapoint> dp);
    std::shared_ptr<Datapoint> find(std::string const& id) const;
    std::vector<std::shared_ptr<Datapoint>> all() const;

private:
    std::map<std::string, std::shared_ptr<Datapoint>> m_byId;
};

} // namespace core::dp
