// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Platform.h"

#include <string>
#include <type_traits>
#include <variant>

#include <drogon/drogon.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <drogon/orm/Field.h>

#include "core/dp/Value.h"

namespace wc {

// Uniform response envelope: { "code":0, "message":"ok", "data":... }.
inline drogon::HttpResponsePtr ok(Json::Value data = Json::Value(Json::nullValue)) {
    Json::Value root;
    root["code"] = 0;
    root["message"] = "ok";
    root["data"] = std::move(data);
    return drogon::HttpResponse::newHttpJsonResponse(root);
}

inline drogon::HttpResponsePtr fail(int code, std::string const& message,
                                    drogon::HttpStatusCode http = drogon::k400BadRequest) {
    Json::Value root;
    root["code"] = code;
    root["message"] = message;
    root["data"] = Json::nullValue;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(http);
    return resp;
}

// A DB row -> JSON object (all columns as strings / null). Good enough for a
// config console; numeric columns arrive as numeric strings.
inline Json::Value rowToJson(drogon::orm::Row const& row) {
    Json::Value o(Json::objectValue);
    for (std::size_t i = 0; i < row.size(); i++) {
        auto const& f = row[i];
        if (f.isNull()) o[f.name()] = Json::nullValue;
        else o[f.name()] = f.as<std::string>();
    }
    return o;
}

inline Json::Value resultToArray(drogon::orm::Result const& r) {
    Json::Value arr(Json::arrayValue);
    for (auto const& row : r) arr.append(rowToJson(row));
    return arr;
}

// core::dp::Value (variant) -> JSON, preserving the scalar type.
inline Json::Value valueToJson(core::dp::Value const& v) {
    return std::visit([](auto const& x) -> Json::Value {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)        return Json::Value(Json::nullValue);
        else if constexpr (std::is_same_v<T, bool>)             return Json::Value(x);
        else if constexpr (std::is_same_v<T, std::string>)      return Json::Value(x);
        else if constexpr (std::is_same_v<T, double>)           return Json::Value(x);
        else if constexpr (std::is_same_v<T, std::int64_t>)     return Json::Value(Json::Int64(x));
        else if constexpr (std::is_same_v<T, std::uint64_t>)    return Json::Value(Json::UInt64(x));
        else                                                    return Json::Value(Json::nullValue);
    }, v);
}

// Serialize a JSON value to a compact string for storage in a *_json column.
// If the value is already a string, use it verbatim.
inline std::string jsonCol(Json::Value const& v) {
    if (v.isNull()) return "{}";
    if (v.isString()) return v.asString();
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}

} // namespace wc
