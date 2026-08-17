// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/control/DeviceRouteManager.h"

#include <set>

namespace core::control {

bool DeviceRouteManager::configure(std::vector<DeviceRoute> routes,
                                   std::string& error) {
    std::map<std::string, DeviceRoute> byId;
    std::map<std::string, std::string> active;
    for (auto& route : routes) {
        if (route.id.empty() || route.deviceId.empty()) {
            error = "route id and device id are required";
            return false;
        }
        if (!byId.emplace(route.id, route).second) {
            error = "duplicate route id '" + route.id + "'";
            return false;
        }
        if (route.active) {
            if (!route.writable) {
                error = "active route '" + route.id + "' is not writable";
                return false;
            }
            if (!active.emplace(route.deviceId, route.id).second) {
                error = "device '" + route.deviceId
                      + "' has more than one active write route";
                return false;
            }
        }
    }
    std::lock_guard lock(m_mtx);
    m_routes = std::move(byId);
    m_activeByDevice = std::move(active);
    return true;
}

bool DeviceRouteManager::setActive(std::string const& deviceId,
                                   std::string const& routeId,
                                   std::string& error) {
    std::lock_guard lock(m_mtx);
    auto const it = m_routes.find(routeId);
    if (it == m_routes.end() || it->second.deviceId != deviceId) {
        error = "route does not belong to device";
        return false;
    }
    if (!it->second.writable) {
        error = "route is not writable";
        return false;
    }
    for (auto& [id, route] : m_routes) {
        if (route.deviceId == deviceId) route.active = id == routeId;
    }
    m_activeByDevice[deviceId] = routeId;
    return true;
}

std::optional<DeviceRoute> DeviceRouteManager::activeRoute(
    std::string const& deviceId) const {
    std::lock_guard lock(m_mtx);
    auto const active = m_activeByDevice.find(deviceId);
    if (active == m_activeByDevice.end()) return std::nullopt;
    auto const route = m_routes.find(active->second);
    return route == m_routes.end() ? std::nullopt
                                  : std::optional<DeviceRoute>(route->second);
}

std::vector<DeviceRoute> DeviceRouteManager::routes() const {
    std::lock_guard lock(m_mtx);
    std::vector<DeviceRoute> out;
    out.reserve(m_routes.size());
    for (auto const& [id, route] : m_routes) {
        (void)id;
        out.push_back(route);
    }
    return out;
}

bool DeviceRouteManager::isActive(std::string const& deviceId,
                                  std::string const& routeId) const {
    std::lock_guard lock(m_mtx);
    auto const it = m_activeByDevice.find(deviceId);
    return it != m_activeByDevice.end() && it->second == routeId;
}

} // namespace core::control
