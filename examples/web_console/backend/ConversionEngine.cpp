// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConversionEngine.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include "core/dp/Value.h"

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

std::mutex g_statMtx;
std::unordered_map<std::string, std::int64_t> g_hits;   // ruleId -> writes

std::string s(Json::Value const& j, char const* k, std::string d = "") {
    return j.isMember(k) && j[k].isString() ? j[k].asString() : d;
}

} // namespace

void startConversionEngine(RuntimeHost& rt) {
    // Rule: source_json {dp}, dest_json {transport, addr}, transform_json {scale}.
    // onChange/periodic both handled by a 1s tick for simplicity.
    app().getLoop()->runEvery(1.0, [&rt] {
        auto db = app().getDbClient();
        if (!db) return;
        Result rules;
        try {
            rules = db->execSqlSync(
                "SELECT id,source_json,dest_json,transform_json FROM conversion_rules WHERE enabled=1");
        } catch (DrogonDbException const&) { return; }
        if (rules.empty()) return;

        auto dps = rt.datapoints();
        for (auto const& r : rules) {
            auto const id = r["id"].as<std::string>();
            auto src = parseJsonOr(r["source_json"].as<std::string>());
            auto dst = parseJsonOr(r["dest_json"].as<std::string>());
            auto tr = parseJsonOr(r["transform_json"].as<std::string>());
            std::string const srcDp = s(src, "dp");
            std::string const destTp = s(dst, "transport");
            if (srcDp.empty() || destTp.empty()) continue;

            double value = 0;
            bool found = false;
            for (auto const& d : dps) {
                if (d.id == srcDp) { value = core::dp::toDouble(d.value); found = true; break; }
            }
            if (!found) continue;

            double const scale = tr.isMember("scale") && tr["scale"].isNumeric() ? tr["scale"].asDouble() : 1.0;
            int const addr = dst.isMember("addr") ? dst["addr"].asInt() : 0;
            auto out = std::uint16_t(std::int64_t(value * scale));

            rt.write(destTp, addr, {out}, [id](bool ok, std::string) {
                if (!ok) return;
                std::lock_guard lk(g_statMtx);
                ++g_hits[id];
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

    app().registerHandler("/api/v1/conversions",
        [db](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            try {
                db()->execSqlSync(
                    "INSERT INTO conversion_rules(id,name,enabled,source_json,dest_json,transform_json,trigger,period_ms) "
                    "VALUES($1,$2,$3,$4,$5,$6,$7,$8)",
                    s(*j, "id"), s(*j, "name"), (*j).get("enabled", 1).asInt(),
                    jsonCol((*j)["source"]), jsonCol((*j)["dest"]), jsonCol((*j)["transform"]),
                    s(*j, "trigger", "onChange"), (*j).get("period_ms", 0).asInt());
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    app().registerHandler("/api/v1/conversions/{id}",
        [db](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            try { db()->execSqlSync("DELETE FROM conversion_rules WHERE id=$1", id); cb(ok()); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Delete});

    auto setEnabled = [db](std::string const& id, int en, std::function<void(HttpResponsePtr const&)>&& cb) {
        try { db()->execSqlSync("UPDATE conversion_rules SET enabled=$1 WHERE id=$2", en, id); cb(ok()); }
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
            std::lock_guard lk(g_statMtx);
            Json::Value d; d["id"] = id; d["hits"] = Json::Int64(g_hits[id]);
            cb(ok(d));
        }, {Get});
}

} // namespace wc
