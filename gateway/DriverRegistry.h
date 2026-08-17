// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/config/ConfigSchema.h"

namespace core::gateway {

struct DriverSnapshot {
    std::string id;
    std::string library;
    std::string state;
    std::string error;
};

class DriverRegistry {
public:
    using DataCallback = std::function<void(
        std::string const&, std::string const&, std::string const&,
        std::vector<std::uint8_t>)>;
    using LogCallback =
        std::function<void(std::string const&, int, std::string const&)>;
    using WriteCallback = std::function<void(bool, std::string)>;

    DriverRegistry();
    ~DriverRegistry();
    DriverRegistry(DriverRegistry const&) = delete;
    DriverRegistry& operator=(DriverRegistry const&) = delete;

    void setDataCallback(DataCallback callback);
    void setLogCallback(LogCallback callback);
    bool load(std::vector<config::DriverConfig> const& drivers,
              std::string const& configDirectory, std::string& error);
    void start();
    void stop();
    bool write(std::string const& driverId, std::string deviceId,
               std::string targetId, std::vector<std::uint8_t> data,
               WriteCallback done);
    std::vector<DriverSnapshot> snapshots() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::gateway
