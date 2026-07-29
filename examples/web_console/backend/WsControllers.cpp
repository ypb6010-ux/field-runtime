// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "WsControllers.h"
#include "AuthControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>

using namespace drogon;

namespace wc {

namespace {

std::mutex g_mtx;
std::set<WebSocketConnectionPtr> g_conns;
constexpr std::size_t kMaxConnections = 256;

using Topics = std::set<std::string>;

struct ClientContext {
    Topics topics;
    std::string token;
};

bool parseJson(std::string const& s, Json::Value& out) {
    Json::CharReaderBuilder b;
    std::string errs;
    auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
    return reader->parse(s.data(), s.data() + s.size(), &out, &errs);
}

bool wants(Topics const& t, std::string const& kind, std::string const& id) {
    return t.count(kind + "/*") || t.count(kind + "/" + id);
}

bool validTopic(std::string const& topic) {
    if (topic == "dp/*" || topic == "transport/*") return true;
    for (auto const* prefix : {"dp/", "transport/"}) {
        if (topic.rfind(prefix, 0) != 0 || topic.size() <= std::strlen(prefix)
            || topic.size() > 256) {
            continue;
        }
        return topic.find_first_of(" \t\r\n") == std::string::npos;
    }
    return false;
}

} // namespace

// /ws/stream — subscribe to "dp/*" | "dp/<id>" | "transport/*" | "transport/<id>".
class StreamWs : public WebSocketController<StreamWs> {
public:
    void handleNewConnection(
        HttpRequestPtr const& request,
        WebSocketConnectionPtr const& c) override {
        auto context = std::make_shared<ClientContext>();
        context->token = request->getParameter("token");
        c->setContext(context);
        bool accepted = false;
        {
            std::lock_guard lock(g_mtx);
            if (g_conns.size() < kMaxConnections) {
                g_conns.insert(c);
                accepted = true;
            }
        }
        if (!accepted) {
            c->shutdown(CloseCode::kViolation, "too many connections");
        }
    }
    void handleNewMessage(WebSocketConnectionPtr const& c, std::string&& msg,
                          WebSocketMessageType const& type) override {
        if (type != WebSocketMessageType::Text) return;
        if (msg.size() > 64 * 1024) {
            c->shutdown(CloseCode::kMessageTooBig, "message too large");
            return;
        }
        Json::Value j;
        if (!parseJson(msg, j) || !j.isObject()) {
            c->send(R"({"type":"error","message":"invalid JSON"})");
            return;
        }
        auto const op = j["op"].asString();
        if (op == "ping") { c->send(R"({"type":"pong"})"); return; }
        if (op == "subscribe" || op == "unsubscribe") {
            if (!j["topics"].isArray() || j["topics"].size() > 256) {
                c->send(R"({"type":"error","message":"invalid topics"})");
                return;
            }
            {
                std::lock_guard lock(g_mtx);
                auto context = c->getContext<ClientContext>();
                if (!context) {
                    context = std::make_shared<ClientContext>();
                    c->setContext(context);
                }
                for (auto const& tpc : j["topics"]) {
                    if (!tpc.isString() || !validTopic(tpc.asString())) continue;
                    if (op == "subscribe") {
                        if (context->topics.size() < 256) {
                            context->topics.insert(tpc.asString());
                        }
                    } else {
                        context->topics.erase(tpc.asString());
                    }
                }
            }
            Json::Value ack; ack["type"] = "ack"; ack["op"] = op;
            c->send(Json::writeString(Json::StreamWriterBuilder(), ack));
        }
    }
    void handleConnectionClosed(WebSocketConnectionPtr const& c) override {
        std::lock_guard lk(g_mtx);
        g_conns.erase(c);
    }
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/stream");
    WS_PATH_LIST_END
};

void startWsPump(RuntimeHost& rt) {
    app().getLoop()->runEvery(1.0, [&rt] {
        auto const dps = rt.datapoints();
        auto const tps = rt.transports();
        struct Recipient {
            WebSocketConnectionPtr connection;
            Topics topics;
            std::string token;
        };
        std::vector<Recipient> recipients;
        {
            std::lock_guard lock(g_mtx);
            recipients.reserve(g_conns.size());
            for (auto const& connection : g_conns) {
                auto context = connection->getContext<ClientContext>();
                if (context) {
                    recipients.push_back(
                        {connection, context->topics, context->token});
                }
            }
        }
        if (recipients.empty()) return;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        for (auto const& [connection, topics, token] : recipients) {
            if (!isSessionTokenValid(token)) {
                connection->shutdown(
                    CloseCode::kViolation,
                    "session expired");
                continue;
            }
            Json::Value msg;
            msg["type"] = "snapshot";
            Json::Value da(Json::arrayValue);
            for (auto const& d : dps) {
                if (!wants(topics, "dp", d.id)) continue;
                Json::Value o; o["id"] = d.id; o["value"] = valueToJson(d.value);
                o["quality"] = d.state; o["ts"] = Json::Int64(d.ts);
                da.append(o);
            }
            Json::Value ta(Json::arrayValue);
            for (auto const& t : tps) {
                if (!wants(topics, "transport", t.id)) continue;
                Json::Value o; o["id"] = t.id; o["kind"] = t.kind; o["state"] = t.state;
                ta.append(o);
            }
            msg["datapoints"] = da;
            msg["transports"] = ta;
            connection->send(Json::writeString(wb, msg));
        }
    });
}

} // namespace wc
