// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "DataControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include "core/dp/Value.h"

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t paramI64(HttpRequestPtr const& req, char const* k, std::int64_t d) {
    auto v = req->getParameter(k);
    if (v.empty()) return d;
    try {
        std::size_t used = 0;
        auto const value = std::stoll(v, &used, 10);
        return used == v.size() ? value : d;
    } catch (...) {
        return d;
    }
}

} // namespace

void startSampler(RuntimeHost& rt) {
    struct SamplerState {
        std::mutex mutex;
        std::unordered_map<std::string, std::int64_t> committedAt;
        std::unordered_map<std::string, std::int64_t> pendingAt;
    };
    auto state = std::make_shared<SamplerState>();
    struct Sample {
        std::string id;
        std::int64_t timestamp = 0;
        std::optional<double> numeric;
        std::string text;
        std::string quality;
    };

    // Sample every 2s on the main loop. Values are grouped into bounded
    // multi-row inserts so SQLite does not open one transaction per datapoint.
    app().getLoop()->runEvery(2.0, [&rt, state] {
        auto db = app().getDbClient();
        if (!db) return;
        auto const ts = nowMs();
        std::unordered_set<std::string> activeIds;
        std::vector<Sample> samples;
        for (auto const& d : rt.datapoints()) {
            activeIds.insert(d.id);
            auto const sampleTs = d.ts ? d.ts : ts;
            {
                std::lock_guard lock(state->mutex);
                auto const committed = state->committedAt[d.id];
                auto const pending = state->pendingAt[d.id];
                if (sampleTs <= std::max(committed, pending)) continue;
                state->pendingAt[d.id] = sampleTs;
            }
            bool const textual =
                std::holds_alternative<std::string>(d.value)
                || std::holds_alternative<std::monostate>(d.value);
            samples.push_back(Sample{
                d.id,
                sampleTs,
                textual
                    ? std::nullopt
                    : std::optional<double>(core::dp::toDouble(d.value)),
                core::dp::toString(d.value),
                d.state,
            });
        }
        {
            std::lock_guard lock(state->mutex);
            std::erase_if(
                state->committedAt,
                [&activeIds](auto const& entry) {
                    return !activeIds.contains(entry.first);
                });
            std::erase_if(
                state->pendingAt,
                [&activeIds](auto const& entry) {
                    return !activeIds.contains(entry.first);
                });
        }

        constexpr std::size_t kRowsPerInsert = 150;
        for (std::size_t begin = 0; begin < samples.size();
             begin += kRowsPerInsert) {
            auto const end =
                std::min(samples.size(), begin + kRowsPerInsert);
            std::string sql =
                "INSERT INTO samples"
                "(dp_id,ts,value_num,value_text,quality) VALUES";
            int parameter = 1;
            for (auto index = begin; index < end; ++index) {
                if (index != begin) sql += ',';
                sql += "($" + std::to_string(parameter++)
                     + ",$" + std::to_string(parameter++)
                     + ",$" + std::to_string(parameter++)
                     + ",$" + std::to_string(parameter++)
                     + ",$" + std::to_string(parameter++) + ')';
            }

            auto binder = *db << std::move(sql);
            std::vector<std::pair<std::string, std::int64_t>> batch;
            batch.reserve(end - begin);
            for (auto index = begin; index < end; ++index) {
                auto const& sample = samples[index];
                batch.emplace_back(sample.id, sample.timestamp);
                binder << sample.id
                       << sample.timestamp
                       << sample.numeric
                       << sample.text
                       << sample.quality;
            }
            binder >> [state, batch](Result const&) {
                std::lock_guard lock(state->mutex);
                for (auto const& [id, timestamp] : batch) {
                    auto pending = state->pendingAt.find(id);
                    if (pending != state->pendingAt.end()
                        && pending->second == timestamp) {
                        state->pendingAt.erase(pending);
                    }
                    auto& committed = state->committedAt[id];
                    committed = std::max(committed, timestamp);
                }
            };
            binder >> [state, batch](DrogonDbException const& error) {
                LOG_WARN << "sample batch insert failed: "
                         << error.base().what();
                std::lock_guard lock(state->mutex);
                for (auto const& [id, timestamp] : batch) {
                    auto pending = state->pendingAt.find(id);
                    if (pending != state->pendingAt.end()
                        && pending->second == timestamp) {
                        state->pendingAt.erase(pending);
                    }
                }
            };
        }
    });

    // Enforce the configured history-retention period. This was previously a
    // dead setting, allowing samples to grow forever.
    app().getLoop()->runEvery(3600.0, [] {
        auto db = app().getDbClient();
        if (!db) return;
        db->execSqlAsync(
            "DELETE FROM samples WHERE ts < "
            "(strftime('%s','now') * 1000 - "
            "COALESCE((SELECT CAST(value_json AS INTEGER) FROM settings "
            "WHERE key='sample_retention_days'),30) * 86400000)",
            [](Result const&) {},
            [](DrogonDbException const& error) {
                LOG_WARN << "sample retention cleanup failed: "
                         << error.base().what();
            });
    });
}

void registerDataControllers(RuntimeHost& rt) {
    // GET /api/v1/data/catalog - read-only metadata needed by monitoring
    // clients. It intentionally omits protocol credentials and scheduler data.
    app().registerHandler("/api/v1/data/catalog",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto rows = app().getDbClient()->execSqlSync(
                    "SELECT id,transport_id,reg_table,addr,type "
                    "FROM datapoints WHERE enabled=1 ORDER BY id");
                cb(ok(resultToArray(rows)));
            } catch (DrogonDbException const& error) {
                cb(fail(
                    4000,
                    error.base().what(),
                    k500InternalServerError));
            }
        }, {Get});

    // GET /api/v1/data/history?id=&from=&to=&page=&size=
    app().registerHandler("/api/v1/data/history",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::string const id = req->getParameter("id");
            if (id.empty()) { cb(fail(1001, "missing id")); return; }
            std::int64_t const from = paramI64(req, "from", 0);
            std::int64_t const to   = paramI64(req, "to", nowMs());
            if (from < 0 || to < 0 || from > to) {
                cb(fail(1001, "from/to must be a valid ascending time range"));
                return;
            }
            auto const page = std::clamp<std::int64_t>(
                paramI64(req, "page", 0), 0, 1'000'000);
            auto const size = std::clamp<std::int64_t>(
                paramI64(req, "size", 200), 1, 5000);
            auto const offset = page * size;
            try {
                auto db = app().getDbClient();
                auto cnt = db->execSqlSync(
                    "SELECT COUNT(*) AS n FROM samples WHERE dp_id=$1 AND ts BETWEEN $2 AND $3",
                    id, from, to);
                auto r = db->execSqlSync(
                    "SELECT ts,value_num,value_text,quality FROM samples "
                    "WHERE dp_id=$1 AND ts BETWEEN $2 AND $3 ORDER BY ts DESC LIMIT $4 OFFSET $5",
                    id, from, to, size, offset);
                Json::Value rows(Json::arrayValue);
                for (auto const& row : r) {
                    Json::Value o;
                    o["ts"] = Json::Int64(row["ts"].as<std::int64_t>());
                    o["value_num"] = row["value_num"].isNull() ? Json::Value(Json::nullValue)
                                                               : Json::Value(row["value_num"].as<double>());
                    o["value_text"] = row["value_text"].as<std::string>();
                    o["quality"] = row["quality"].as<std::string>();
                    rows.append(o);
                }
                Json::Value data;
                data["total"] = Json::Int64(cnt[0]["n"].as<std::int64_t>());
                data["page"] = Json::Int64(page);
                data["size"] = Json::Int64(size);
                data["rows"] = rows;
                cb(ok(data));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            }
        }, {Get});

    // POST /api/v1/data/write  { target_id, values:[u16,..] }
    // All writes use a logical control target so no Web endpoint can bypass
    // actor resolution, arbitration, or the device's active write route.
    app().registerHandler("/api/v1/data/write",
        [&rt](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            if (!(*j)["target_id"].isString() || !(*j)["values"].isArray()) {
                cb(fail(1001, "target_id and values[] are required"));
                return;
            }
            std::string const targetId = (*j)["target_id"].asString();
            if (targetId.empty()) {
                cb(fail(1001, "target_id is required"));
                return;
            }
            std::vector<std::uint8_t> payload;
            auto const& input = (*j)["values"];
            if (input.empty() || input.size() > 123) {
                cb(fail(1001, "values must contain 1..123 registers"));
                return;
            }
            payload.reserve(input.size() * 2);
            for (auto const& value : input) {
                if (!value.isIntegral()) {
                    cb(fail(1001, "register values must be integers"));
                    return;
                }
                auto const parsed = value.asInt64();
                if (parsed < 0 || parsed > 65535) {
                    cb(fail(1001, "register values must be in 0..65535"));
                    return;
                }
                payload.push_back(static_cast<std::uint8_t>(parsed >> 8));
                payload.push_back(static_cast<std::uint8_t>(parsed & 0xFF));
            }
            auto cbp = std::make_shared<std::function<void(HttpResponsePtr const&)>>(std::move(cb));
            rt.writeControl(
                req->attributes()->get<std::string>("auth_username"),
                targetId,
                std::move(payload),
                [cbp](bool okay, std::string err) {
                (*cbp)(okay ? ok() : fail(4001, err.empty() ? "write failed" : err));
            });
        }, {Post});
}

} // namespace wc
