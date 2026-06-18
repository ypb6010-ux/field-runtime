// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "DataControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
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
    try { return std::stoll(v); } catch (...) { return d; }
}

} // namespace

void startSampler(RuntimeHost& rt) {
    // Sample every 2s on the main loop; non-blocking async inserts.
    app().getLoop()->runEvery(2.0, [&rt] {
        auto db = app().getDbClient();
        if (!db) return;
        auto const ts = nowMs();
        for (auto const& d : rt.datapoints()) {
            db->execSqlAsync(
                "INSERT INTO samples(dp_id,ts,value_num,value_text,quality) VALUES($1,$2,$3,$4,$5)",
                [](Result const&) {}, [](DrogonDbException const&) {},
                d.id, d.ts ? d.ts : ts, core::dp::toDouble(d.value),
                core::dp::toString(d.value), d.state);
        }
    });
}

void registerDataControllers(RuntimeHost& rt) {
    // GET /api/v1/data/history?id=&from=&to=&page=&size=
    app().registerHandler("/api/v1/data/history",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::string const id = req->getParameter("id");
            if (id.empty()) { cb(fail(1001, "missing id")); return; }
            std::int64_t const from = paramI64(req, "from", 0);
            std::int64_t const to   = paramI64(req, "to", nowMs());
            int const page = std::max(0, int(paramI64(req, "page", 0)));
            int const size = std::clamp(int(paramI64(req, "size", 200)), 1, 5000);
            try {
                auto db = app().getDbClient();
                auto cnt = db->execSqlSync(
                    "SELECT COUNT(*) AS n FROM samples WHERE dp_id=$1 AND ts BETWEEN $2 AND $3",
                    id, from, to);
                auto r = db->execSqlSync(
                    "SELECT ts,value_num,value_text,quality FROM samples "
                    "WHERE dp_id=$1 AND ts BETWEEN $2 AND $3 ORDER BY ts DESC LIMIT $4 OFFSET $5",
                    id, from, to, size, page * size);
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
                data["page"] = page;
                data["size"] = size;
                data["rows"] = rows;
                cb(ok(data));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            }
        }, {Get});

    // POST /api/v1/data/write  { transport, addr, values:[..] }
    app().registerHandler("/api/v1/data/write",
        [&rt](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            std::string const transport = (*j)["transport"].asString();
            int const addr = (*j)["addr"].asInt();
            std::vector<std::uint16_t> values;
            for (auto const& v : (*j)["values"]) values.push_back(std::uint16_t(v.asUInt()));
            if (transport.empty() || values.empty()) { cb(fail(1001, "transport/values required")); return; }
            auto cbp = std::make_shared<std::function<void(HttpResponsePtr const&)>>(std::move(cb));
            rt.write(transport, addr, std::move(values), [cbp](bool okay, std::string err) {
                (*cbp)(okay ? ok() : fail(4001, err.empty() ? "write failed" : err));
            });
        }, {Post});
}

} // namespace wc
