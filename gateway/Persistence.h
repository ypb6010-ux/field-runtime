// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/dp/State.h"
#include "core/dp/Value.h"

struct sqlite3;

namespace core::gateway {

struct PersistenceConfig {
    bool enable = false;
    std::string path = "field_gateway.db";
    int maxRows = 100000;
    int backfillBatch = 100;
};

struct TelemetryRow {
    std::int64_t rowId = 0;
    std::string dpId;
    std::string valueJson;
    std::string quality;
    std::int64_t ts = 0;
};

class Persistence {
public:
    Persistence();
    ~Persistence();

    Persistence(Persistence const&) = delete;
    Persistence& operator=(Persistence const&) = delete;

    bool open(PersistenceConfig config, std::string& error);
    void close();

    bool enabled() const;
    PersistenceConfig const& config() const;

    std::optional<std::int64_t> insertTelemetry(std::string const& dpId,
                                                dp::Value const& value,
                                                dp::DpState state,
                                                dp::Timestamp timestamp,
                                                std::string& error);
    bool markPublished(std::int64_t rowId, std::string& error);
    std::vector<TelemetryRow> pendingTelemetry(int limit, std::string& error);
    bool prune(std::string& error);
    int pendingCount(std::string& error) const;

private:
    bool exec(char const* sql, std::string& error);

    sqlite3* m_db = nullptr;
    PersistenceConfig m_config;
};

} // namespace core::gateway
