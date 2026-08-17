// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/core_global.h"

namespace core::control {

struct DeviceRoute {
    std::string id;
    std::string deviceId;
    std::string driverId;
    std::string transportId;
    std::string protocol;
    bool writable = true;
    bool active = false;
};

class CORE_EXPORT DeviceRouteManager {
public:
    bool configure(std::vector<DeviceRoute> routes, std::string& error);
    bool setActive(std::string const& deviceId,
                   std::string const& routeId,
                   std::string& error);
    std::optional<DeviceRoute> activeRoute(std::string const& deviceId) const;
    std::vector<DeviceRoute> routes() const;
    bool isActive(std::string const& deviceId,
                  std::string const& routeId) const;

private:
    mutable std::mutex m_mtx;
    std::map<std::string, DeviceRoute> m_routes;
    std::map<std::string, std::string> m_activeByDevice;
};

} // namespace core::control
