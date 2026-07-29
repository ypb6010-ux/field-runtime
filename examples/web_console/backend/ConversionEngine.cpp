// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConversionEngine.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <mutex>
#include <memory>
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

struct RuleState {
    std::optional<double> lastSuccessfulValue;
    std::optional<double> lastAttemptedValue;
    std::chrono::steady_clock::time_point lastAttempt{};
    bool inFlight = false;
    std::int64_t hits = 0;
    std::int64_t failures = 0;
    std::int64_t skipped = 0;
    std::string lastError;
    std::int64_t lastRunMs = 0;
};

struct RuleDefinition {
    std::string id;
    std::string sourceDatapoint;
    std::string destinationTransport;
    double scale = 1.0;
    int address = 0;
    int periodMs = 100;
    bool enabled = false;
    bool periodic = false;
};

std::mutex g_stateMtx;
std::unordered_map<std::string, RuleState> g_states;
std::atomic_uint64_t g_rulesRevision{1};

std::string s(Json::Value const& j, char const* k, std::string d = "") {
    return j.isMember(k) && j[k].isString() ? j[k].asString() : d;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool validId(std::string const& id) {
    return !id.empty() && id.size() <= 128
        && id.find_first_of(" \t\r\n/\\?#%") == std::string::npos;
}

std::optional<std::string> validateRule(Json::Value const& j,
                                        bool requireId) {
    if (requireId && !validId(s(j, "id"))) {
        return "id must contain 1..128 path-safe non-whitespace characters";
    }
    if (s(j, "name").size() > 256) return "name is too long";
    auto const& source = j["source"];
    auto const& destination = j["dest"];
    auto const& transform = j["transform"];
    if (!source.isObject() || !validId(s(source, "dp"))) {
        return "source.dp is required";
    }
    if (!destination.isObject()
        || !validId(s(destination, "transport"))
        || !destination["addr"].isIntegral()) {
        return "dest.transport and integer dest.addr are required";
    }
    auto const address = destination["addr"].asInt64();
    if (address < 0 || address > 65535) {
        return "dest.addr must be in 0..65535";
    }
    if (!transform.isObject()) return "transform must be an object";
    auto const scale = transform.get("scale", 1.0);
    if (!scale.isNumeric() || !std::isfinite(scale.asDouble())
        || scale.asDouble() == 0.0) {
        return "transform.scale must be finite and non-zero";
    }
    auto const trigger = s(j, "trigger", "onChange");
    if (trigger != "onChange" && trigger != "periodic") {
        return "trigger must be onChange or periodic";
    }
    auto const& period = j["period_ms"];
    if (trigger == "periodic"
        && (!period.isIntegral() || period.asInt64() < 100
            || period.asInt64() > 86400000)) {
        return "periodic rules require period_ms in 100..86400000";
    }
    auto const& enabled = j["enabled"];
    if (!enabled.isNull() && !enabled.isBool()
        && !(enabled.isIntegral()
             && (enabled.asInt64() == 0 || enabled.asInt64() == 1))) {
        return "enabled must be boolean";
    }
    return std::nullopt;
}

bool referencedObjectsExist(DbClientPtr const& database,
                            Json::Value const& rule) {
    auto source = database->execSqlSync(
        "SELECT 1 FROM datapoints WHERE id=$1",
        s(rule["source"], "dp"));
    auto destination = database->execSqlSync(
        "SELECT 1 FROM transports WHERE id=$1",
        s(rule["dest"], "transport"));
    return !source.empty() && !destination.empty();
}

Json::Value stateJson(std::string const& id, RuleState const& state) {
    Json::Value data;
    data["id"] = id;
    data["hits"] = Json::Int64(state.hits);
    data["failures"] = Json::Int64(state.failures);
    data["skipped"] = Json::Int64(state.skipped);
    data["lastRunMs"] = Json::Int64(state.lastRunMs);
    data["lastError"] = state.lastError;
    data["inFlight"] = state.inFlight;
    return data;
}

} // namespace

void startConversionEngine(RuntimeHost& rt) {
    // Rule: source_json {dp}, dest_json {transport, addr}, transform_json
    // {scale}. onChange writes once per new source value; periodic obeys its
    // configured period. A per-rule in-flight gate prevents overlapping device
    // writes when a transport is slow.
    auto rules = std::make_shared<std::vector<RuleDefinition>>();
    auto lastRefresh = std::make_shared<std::chrono::steady_clock::time_point>();
    auto loadedRevision = std::make_shared<std::uint64_t>(0);
    app().getLoop()->runEvery(
        0.2,
        [&rt, rules, lastRefresh, loadedRevision] {
        auto db = app().getDbClient();
        if (!db) return;
        auto const now = std::chrono::steady_clock::now();
        auto const revision = g_rulesRevision.load(std::memory_order_acquire);
        if (*loadedRevision != revision
            || lastRefresh->time_since_epoch().count() == 0
            || now - *lastRefresh >= std::chrono::seconds(1)) {
            *lastRefresh = now;
            try {
                std::vector<RuleDefinition> refreshed;
                auto rows = db->execSqlSync(
                    "SELECT id,source_json,dest_json,transform_json,trigger,"
                    "period_ms,enabled FROM conversion_rules");
                refreshed.reserve(rows.size());
                for (auto const& row : rows) {
                    auto const source =
                        parseJsonOr(row["source_json"].as<std::string>());
                    auto const destination =
                        parseJsonOr(row["dest_json"].as<std::string>());
                    auto const transform =
                        parseJsonOr(row["transform_json"].as<std::string>());
                    RuleDefinition rule;
                    rule.id = row["id"].as<std::string>();
                    rule.sourceDatapoint = s(source, "dp");
                    rule.destinationTransport = s(destination, "transport");
                    rule.scale =
                        transform.isMember("scale")
                            && transform["scale"].isNumeric()
                        ? transform["scale"].asDouble()
                        : 1.0;
                    rule.address = destination.get("addr", 0).asInt();
                    rule.periodMs =
                        std::max(100, row["period_ms"].as<int>());
                    rule.enabled = row["enabled"].as<int>() != 0;
                    rule.periodic =
                        row["trigger"].as<std::string>() == "periodic";
                    refreshed.push_back(std::move(rule));
                }
                *rules = std::move(refreshed);
                *loadedRevision = revision;
            } catch (DrogonDbException const& error) {
                LOG_WARN << "conversion rules refresh failed: "
                         << error.base().what();
            } catch (std::exception const& error) {
                LOG_WARN << "conversion rules parse failed: " << error.what();
            }
        }

        auto dps = rt.datapoints();
        std::unordered_map<std::string, double> healthyValues;
        healthyValues.reserve(dps.size());
        for (auto const& datapoint : dps) {
            if (datapoint.state != "Ok") continue;
            auto const value = core::dp::toDouble(datapoint.value);
            if (std::isfinite(value)) {
                healthyValues.emplace(datapoint.id, value);
            }
        }
        std::unordered_set<std::string> activeIds;
        for (auto const& rule : *rules) {
            activeIds.insert(rule.id);
            if (!rule.enabled || rule.sourceDatapoint.empty()
                || rule.destinationTransport.empty()) {
                continue;
            }
            auto const input = healthyValues.find(rule.sourceDatapoint);
            if (input == healthyValues.end()) continue;
            auto const value = input->second;
            {
                std::lock_guard lock(g_stateMtx);
                auto& state = g_states[rule.id];
                if (state.inFlight) {
                    ++state.skipped;
                    continue;
                }
                bool const due =
                    rule.periodic
                        ? state.lastAttempt.time_since_epoch().count() == 0
                              || now - state.lastAttempt
                                     >= std::chrono::milliseconds(rule.periodMs)
                        : !state.lastAttemptedValue.has_value()
                              || *state.lastAttemptedValue != value
                              || (!state.lastError.empty()
                                  && now - state.lastAttempt
                                         >= std::chrono::seconds(5));
                if (!due) continue;
                state.inFlight = true;
                state.lastAttempt = now;
                state.lastAttemptedValue = value;
            }

            auto const transformed = value * rule.scale;
            if (!std::isfinite(transformed) || transformed < 0.0
                || transformed > 65535.0) {
                std::lock_guard lock(g_stateMtx);
                auto& state = g_states[rule.id];
                state.inFlight = false;
                ++state.failures;
                state.lastError = "converted value is outside 0..65535";
                state.lastRunMs = nowMs();
                continue;
            }

            auto const out = static_cast<std::uint16_t>(
                std::llround(transformed));
            rt.write(
                rule.destinationTransport,
                rule.address,
                {out},
                [id = rule.id, value](bool ok, std::string error) {
                    std::lock_guard lock(g_stateMtx);
                    auto& state = g_states[id];
                    state.inFlight = false;
                    state.lastRunMs = nowMs();
                    if (ok) {
                        ++state.hits;
                        state.lastSuccessfulValue = value;
                        state.lastError.clear();
                    } else {
                        ++state.failures;
                        state.lastError =
                            error.empty() ? "write failed" : std::move(error);
                    }
                });
        }
        {
            std::lock_guard lock(g_stateMtx);
            std::erase_if(g_states, [&activeIds](auto const& entry) {
                return !activeIds.count(entry.first) && !entry.second.inFlight;
            });
        }
        });
}

void registerConversionControllers() {
    auto db = [] { return app().getDbClient(); };

    app().registerHandler("/api/v1/conversions",
        [db](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try { cb(ok(resultToArray(db()->execSqlSync("SELECT * FROM conversion_rules")))); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    app().registerHandler(
        "/api/v1/conversions/stats",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value data(Json::arrayValue);
            std::lock_guard lock(g_stateMtx);
            for (auto const& [id, state] : g_states) {
                data.append(stateJson(id, state));
            }
            cb(ok(data));
        },
        {Get});

    app().registerHandler("/api/v1/conversions",
        [db](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            if (auto error = validateRule(*j, true)) {
                cb(fail(1001, *error));
                return;
            }
            try {
                if (!referencedObjectsExist(db(), *j)) {
                    cb(fail(
                        1001,
                        "source datapoint or destination transport does not exist"));
                    return;
                }
                db()->execSqlSync(
                    "INSERT INTO conversion_rules(id,name,enabled,source_json,dest_json,transform_json,trigger,period_ms) "
                    "VALUES($1,$2,$3,$4,$5,$6,$7,$8)",
                    s(*j, "id"), s(*j, "name"),
                    (*j).get("enabled", true).asBool() ? 1 : 0,
                    jsonCol((*j)["source"]), jsonCol((*j)["dest"]), jsonCol((*j)["transform"]),
                    s(*j, "trigger", "onChange"), (*j).get("period_ms", 0).asInt());
                g_rulesRevision.fetch_add(1, std::memory_order_release);
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    app().registerHandler("/api/v1/conversions/{id}",
        [db](HttpRequestPtr const& req,
             std::function<void(HttpResponsePtr const&)>&& cb,
             std::string id) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            if (auto error = validateRule(*j, false)) {
                cb(fail(1001, *error));
                return;
            }
            try {
                if (!referencedObjectsExist(db(), *j)) {
                    cb(fail(
                        1001,
                        "source datapoint or destination transport does not exist"));
                    return;
                }
                auto result = db()->execSqlSync(
                    "UPDATE conversion_rules SET name=$1,enabled=$2,"
                    "source_json=$3,dest_json=$4,transform_json=$5,trigger=$6,"
                    "period_ms=$7 WHERE id=$8",
                    s(*j, "name"),
                    (*j).get("enabled", true).asBool() ? 1 : 0,
                    jsonCol((*j)["source"]),
                    jsonCol((*j)["dest"]),
                    jsonCol((*j)["transform"]),
                    s(*j, "trigger", "onChange"),
                    (*j).get("period_ms", 0).asInt(),
                    id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                g_rulesRevision.fetch_add(1, std::memory_order_release);
                cb(ok());
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            }
        }, {Put});

    app().registerHandler("/api/v1/conversions/{id}",
        [db](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            try {
                auto result = db()->execSqlSync(
                    "DELETE FROM conversion_rules WHERE id=$1", id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                g_rulesRevision.fetch_add(1, std::memory_order_release);
                {
                    std::lock_guard lock(g_stateMtx);
                    auto it = g_states.find(id);
                    if (it != g_states.end() && !it->second.inFlight) {
                        g_states.erase(it);
                    }
                }
                cb(ok());
            }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Delete});

    auto setEnabled = [db](std::string const& id, int en, std::function<void(HttpResponsePtr const&)>&& cb) {
        try {
            auto result = db()->execSqlSync(
                "UPDATE conversion_rules SET enabled=$1 WHERE id=$2", en, id);
            if (result.affectedRows() == 0) {
                cb(fail(1404, "not found", k404NotFound));
                return;
            }
            g_rulesRevision.fetch_add(1, std::memory_order_release);
            cb(ok());
        }
        catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
    };
    app().registerHandler("/api/v1/conversions/{id}/enable",
        [setEnabled](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            setEnabled(id, 1, std::move(cb));
        }, {Post});
    app().registerHandler("/api/v1/conversions/{id}/disable",
        [setEnabled](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            setEnabled(id, 0, std::move(cb));
        }, {Post});

    app().registerHandler("/api/v1/conversions/{id}/stats",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            std::lock_guard lock(g_stateMtx);
            auto const it = g_states.find(id);
            RuleState const state =
                it == g_states.end() ? RuleState{} : it->second;
            cb(ok(stateJson(id, state)));
        }, {Get});
}

} // namespace wc
