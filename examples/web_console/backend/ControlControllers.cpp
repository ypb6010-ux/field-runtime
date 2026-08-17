// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ControlControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

using namespace drogon;
using namespace drogon::orm;

namespace wc {
namespace {

Json::Value rows(Result const& result,
                 std::initializer_list<char const*> columns) {
    Json::Value out(Json::arrayValue);
    for (auto const& row : result) {
        Json::Value item;
        for (auto const* column : columns) {
            if (row[column].isNull()) item[column] = Json::Value(Json::nullValue);
            else item[column] = row[column].as<std::string>();
        }
        out.append(item);
    }
    return out;
}

std::string string(Json::Value const& value, char const* key) {
    return value[key].isString() ? value[key].asString() : std::string{};
}

int integer(Json::Value const& value, char const* key, int fallback = 0) {
    return value[key].isIntegral() ? value[key].asInt() : fallback;
}

bool boolean(Json::Value const& value, char const* key, bool fallback) {
    return value[key].isBool() ? value[key].asBool()
         : value[key].isIntegral() ? value[key].asInt() != 0 : fallback;
}

Json::Value loadConfig(DbClientPtr const& db) {
    Json::Value out;
    out["drivers"] = rows(db->execSqlSync(
        "SELECT id,library,config,enabled FROM driver_catalog ORDER BY id"),
        {"id","library","config","enabled"});
    out["actors"] = rows(db->execSqlSync(
        "SELECT id,channel,client_id,source_address,role,priority,enabled "
        "FROM control_actors ORDER BY id"),
        {"id","channel","client_id","source_address","role","priority","enabled"});
    out["servers"] = rows(db->execSqlSync(
        "SELECT id,name,listen_address,listen_port,max_clients,range_start,"
        "range_count,enabled FROM modbus_servers ORDER BY id"),
        {"id","name","listen_address","listen_port","max_clients",
         "range_start","range_count","enabled"});
    out["bridges"] = rows(db->execSqlSync(
        "SELECT id,server_id,plc_transport_id,offset,write_start,write_count,"
        "mirror_start,mirror_count,mirror_policy,mirror_period_ms "
        "FROM control_bridges ORDER BY id"),
        {"id","server_id","plc_transport_id","offset","write_start",
         "write_count","mirror_start","mirror_count","mirror_policy",
         "mirror_period_ms"});
    out["devices"] = rows(db->execSqlSync(
        "SELECT id,name,driver_id FROM devices ORDER BY id"),
        {"id","name","driver_id"});
    out["routes"] = rows(db->execSqlSync(
        "SELECT id,device_id,protocol,transport_id,driver_id,writable,active "
        "FROM device_routes ORDER BY device_id,id"),
        {"id","device_id","protocol","transport_id","driver_id","writable","active"});
    out["targets"] = rows(db->execSqlSync(
        "SELECT id,device_id,route_id,protocol,endpoint,resource,selector,"
        "offset,width,mask FROM control_targets ORDER BY device_id,id"),
        {"id","device_id","route_id","protocol","endpoint","resource",
         "selector","offset","width","mask"});
    out["policies"] = rows(db->execSqlSync(
        "SELECT id,target_id,mode,lease_ms,min_priority FROM control_policies "
        "ORDER BY target_id"),
        {"id","target_id","mode","lease_ms","min_priority"});
    auto mqtt = db->execSqlSync(
        "SELECT value_json FROM settings WHERE key='northbound_mqtt'");
    out["mqtt"] = mqtt.empty()
        ? Json::Value(Json::objectValue)
        : parseJsonOr(mqtt[0]["value_json"].as<std::string>(),
                      Json::Value(Json::objectValue));
    return out;
}

void replaceConfig(DbClientPtr const& db, Json::Value const& root) {
    for (auto const* key : {"drivers","actors","servers","bridges","devices",
                            "routes","targets","policies"}) {
        if (!root[key].isArray()) throw std::runtime_error(std::string(key) + " must be an array");
    }
    if (!root["mqtt"].isObject()) {
        throw std::runtime_error("mqtt must be an object");
    }
    db->execSqlSync("BEGIN IMMEDIATE");
    try {
        db->execSqlSync("DELETE FROM control_policies");
        db->execSqlSync("DELETE FROM control_targets");
        db->execSqlSync("DELETE FROM device_routes");
        db->execSqlSync("DELETE FROM devices");
        db->execSqlSync("DELETE FROM control_bridges");
        db->execSqlSync("DELETE FROM modbus_servers");
        db->execSqlSync("DELETE FROM control_actors");
        db->execSqlSync("DELETE FROM driver_catalog");
        for (auto const& item : root["drivers"]) {
            db->execSqlSync("INSERT INTO driver_catalog(id,library,config,enabled) VALUES($1,$2,$3,$4)",
                string(item,"id"), string(item,"library"), string(item,"config"), boolean(item,"enabled",true));
        }
        for (auto const& item : root["actors"]) {
            db->execSqlSync("INSERT INTO control_actors(id,channel,client_id,source_address,role,priority,enabled) VALUES($1,$2,$3,$4,$5,$6,$7)",
                string(item,"id"), string(item,"channel"), string(item,"client_id"),
                string(item,"source_address"), string(item,"role"), integer(item,"priority"),
                boolean(item,"enabled",true));
        }
        for (auto const& item : root["servers"]) {
            db->execSqlSync("INSERT INTO modbus_servers(id,name,listen_address,listen_port,max_clients,range_start,range_count,enabled) VALUES($1,$2,$3,$4,$5,$6,$7,$8)",
                string(item,"id"), string(item,"name"), string(item,"listen_address"),
                integer(item,"listen_port",502), integer(item,"max_clients",4),
                integer(item,"range_start"), integer(item,"range_count",1),
                boolean(item,"enabled",true));
        }
        for (auto const& item : root["bridges"]) {
            db->execSqlSync("INSERT INTO control_bridges(id,server_id,plc_transport_id,offset,write_start,write_count,mirror_start,mirror_count,mirror_policy,mirror_period_ms) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                string(item,"id"), string(item,"server_id"), string(item,"plc_transport_id"),
                integer(item,"offset"), integer(item,"write_start"), integer(item,"write_count"),
                integer(item,"mirror_start"), integer(item,"mirror_count"),
                string(item,"mirror_policy"), integer(item,"mirror_period_ms"));
        }
        for (auto const& item : root["devices"]) {
            auto const driver = string(item,"driver_id");
            db->execSqlSync("INSERT INTO devices(id,name,driver_id) VALUES($1,$2,NULLIF($3,''))",
                string(item,"id"), string(item,"name"), driver);
        }
        for (auto const& item : root["routes"]) {
            db->execSqlSync("INSERT INTO device_routes(id,device_id,protocol,transport_id,driver_id,writable,active) VALUES($1,$2,$3,NULLIF($4,''),NULLIF($5,''),$6,$7)",
                string(item,"id"), string(item,"device_id"), string(item,"protocol"),
                string(item,"transport_id"), string(item,"driver_id"),
                boolean(item,"writable",true), boolean(item,"active",false));
        }
        for (auto const& item : root["targets"]) {
            db->execSqlSync("INSERT INTO control_targets(id,device_id,route_id,protocol,endpoint,resource,selector,offset,width,mask) VALUES($1,$2,NULLIF($3,''),$4,$5,$6,$7,$8,$9,$10)",
                string(item,"id"), string(item,"device_id"), string(item,"route_id"),
                string(item,"protocol"), string(item,"endpoint"), string(item,"resource"),
                string(item,"selector"), integer(item,"offset"), integer(item,"width",1),
                item["mask"].isIntegral() ? item["mask"].asInt64() : std::int64_t(-1));
        }
        for (auto const& item : root["policies"]) {
            db->execSqlSync("INSERT INTO control_policies(id,target_id,mode,lease_ms,min_priority) VALUES($1,$2,$3,$4,$5)",
                string(item,"id"), string(item,"target_id"), string(item,"mode"),
                integer(item,"lease_ms"), integer(item,"min_priority"));
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        db->execSqlSync(
            "INSERT INTO settings(key,value_json) VALUES('northbound_mqtt',$1) "
            "ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json",
            Json::writeString(writer, root["mqtt"]));
        db->execSqlSync("COMMIT");
    } catch (...) {
        try { db->execSqlSync("ROLLBACK"); } catch (...) {}
        throw;
    }
}

} // namespace

void registerControlControllers(RuntimeHost& runtime) {
    app().registerHandler("/api/v1/control/config",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try { cb(ok(loadConfig(app().getDbClient()))); }
            catch (std::exception const& e) { cb(fail(4000, e.what(), k500InternalServerError)); }
        }, {Get});

    app().registerHandler("/api/v1/control/config",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto body = req->getJsonObject();
            if (!body || !body->isObject()) { cb(fail(1001, "JSON object required")); return; }
            try { replaceConfig(app().getDbClient(), *body); cb(ok(loadConfig(app().getDbClient()))); }
            catch (std::exception const& e) { cb(fail(1001, e.what(), k400BadRequest)); }
        }, {Put});

    app().registerHandler("/api/v1/control/runtime",
        [&runtime](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value out;
            out["running"] = runtime.running();
            out["drivers"] = Json::Value(Json::arrayValue);
            for (auto const& driver : runtime.drivers()) {
                Json::Value value; value["id"] = driver.id; value["library"] = driver.library;
                value["state"] = driver.state; value["error"] = driver.error;
                out["drivers"].append(value);
            }
            out["routes"] = Json::Value(Json::arrayValue);
            for (auto const& route : runtime.routes()) {
                Json::Value value; value["id"] = route.id; value["device_id"] = route.deviceId;
                value["protocol"] = route.protocol; value["active"] = route.active;
                value["writable"] = route.writable; out["routes"].append(value);
            }
            out["leases"] = Json::Value(Json::arrayValue);
            for (auto const& lease : runtime.leases()) {
                Json::Value value; value["target_id"] = lease.targetId; value["actor_id"] = lease.actorId;
                value["priority"] = lease.priority; value["expires_at"] = Json::Int64(lease.expiresAtMs);
                out["leases"].append(value);
            }
            out["data"] = Json::Value(Json::arrayValue);
            for (auto const& data : runtime.driverData()) {
                Json::Value value; value["driver_id"] = data.driverId; value["device_id"] = data.deviceId;
                value["target_id"] = data.targetId; value["ts"] = Json::Int64(data.timestampMs);
                value["payload"] = Json::Value(Json::arrayValue);
                for (auto byte : data.payload) value["payload"].append(byte);
                out["data"].append(value);
            }
            cb(ok(out));
        }, {Get});

    app().registerHandler("/api/v1/control/routes/activate",
        [&runtime](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto body = req->getJsonObject();
            if (!body) { cb(fail(1001, "JSON body required")); return; }
            auto const deviceId = string(*body,"device_id");
            auto const routeId = string(*body,"route_id");
            if (deviceId.empty() || routeId.empty()) {
                cb(fail(1001, "device_id and route_id are required"));
                return;
            }
            try {
            auto database = app().getDbClient();
            auto route = database->execSqlSync(
                "SELECT id FROM device_routes WHERE id=$1 AND device_id=$2 "
                "AND writable=1", routeId, deviceId);
            if (route.empty()) {
                cb(fail(1001, "writable route does not belong to device"));
                return;
            }
            auto previous = database->execSqlSync(
                "SELECT id FROM device_routes WHERE device_id=$1 AND active=1",
                deviceId);
            auto const previousId = previous.empty()
                ? std::string{} : previous[0]["id"].as<std::string>();
            database->execSqlSync("BEGIN IMMEDIATE");
            try {
                database->execSqlSync(
                    "UPDATE device_routes SET active=0 WHERE device_id=$1",
                    deviceId);
                database->execSqlSync(
                    "UPDATE device_routes SET active=1 WHERE id=$1", routeId);
                database->execSqlSync("COMMIT");
            } catch (...) {
                try { database->execSqlSync("ROLLBACK"); } catch (...) {}
                throw;
            }
            auto completion = std::make_shared<std::function<void(HttpResponsePtr const&)>>(std::move(cb));
            runtime.activateRoute(deviceId, routeId,
                [database, completion, deviceId, previousId](
                    bool success, std::string error) {
                    if (!success) {
                        try {
                            database->execSqlSync("BEGIN IMMEDIATE");
                            database->execSqlSync(
                                "UPDATE device_routes SET active=0 WHERE device_id=$1",
                                deviceId);
                            if (!previousId.empty()) {
                                database->execSqlSync(
                                    "UPDATE device_routes SET active=1 WHERE id=$1",
                                    previousId);
                            }
                            database->execSqlSync("COMMIT");
                        } catch (...) {
                            try { database->execSqlSync("ROLLBACK"); } catch (...) {}
                            error += "; failed to restore persisted route";
                        }
                    }
                    (*completion)(success ? ok() : fail(4001, error.empty() ? "route activation failed" : error));
                });
            } catch (std::exception const& e) {
                cb(fail(4000, e.what(), k500InternalServerError));
            }
        }, {Post});

    app().registerHandler("/api/v1/control/write",
        [&runtime](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto body = req->getJsonObject();
            if (!body || !(*body)["payload"].isArray()) { cb(fail(1001, "target_id and payload[] are required")); return; }
            std::vector<std::uint8_t> payload;
            for (auto const& value : (*body)["payload"]) {
                if (!value.isIntegral() || value.asInt64() < 0 || value.asInt64() > 255) {
                    cb(fail(1001, "payload values must be bytes")); return;
                }
                payload.push_back(static_cast<std::uint8_t>(value.asUInt()));
            }
            auto actor = req->attributes()->get<std::string>("auth_username");
            auto completion = std::make_shared<std::function<void(HttpResponsePtr const&)>>(std::move(cb));
            runtime.writeControl(actor, string(*body,"target_id"), std::move(payload),
                [completion](bool success, std::string error) {
                    (*completion)(success ? ok() : fail(4001, error.empty() ? "write failed" : error));
                });
        }, {Post});
}

} // namespace wc
