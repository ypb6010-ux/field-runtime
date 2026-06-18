// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "WsControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>

using namespace drogon;

namespace wc {

namespace {

std::mutex g_mtx;
std::set<WebSocketConnectionPtr> g_conns;

using Topics = std::set<std::string>;

bool parseJson(std::string const& s, Json::Value& out) {
    Json::CharReaderBuilder b;
    std::string errs;
    auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
    return reader->parse(s.data(), s.data() + s.size(), &out, &errs);
}

bool wants(Topics const& t, std::string const& kind, std::string const& id) {
    return t.count(kind + "/*") || t.count(kind + "/" + id);
}

} // namespace

// /ws/stream — subscribe to "dp/*" | "dp/<id>" | "transport/*" | "transport/<id>".
class StreamWs : public WebSocketController<StreamWs> {
public:
    void handleNewConnection(HttpRequestPtr const&, WebSocketConnectionPtr const& c) override {
        c->setContext(std::make_shared<Topics>(Topics{"dp/*", "transport/*"}));  // default: all
        std::lock_guard lk(g_mtx);
        g_conns.insert(c);
    }
    void handleNewMessage(WebSocketConnectionPtr const& c, std::string&& msg,
                          WebSocketMessageType const& type) override {
        if (type != WebSocketMessageType::Text) return;
        Json::Value j;
        if (!parseJson(msg, j)) return;
        auto const op = j["op"].asString();
        if (op == "ping") { c->send(R"({"type":"pong"})"); return; }
        if (op == "subscribe" || op == "unsubscribe") {
            auto topics = c->getContext<Topics>();
            if (!topics) { topics = std::make_shared<Topics>(); c->setContext(topics); }
            for (auto const& tpc : j["topics"]) {
                if (op == "subscribe") topics->insert(tpc.asString());
                else topics->erase(tpc.asString());
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
        std::lock_guard lk(g_mtx);
        if (g_conns.empty()) return;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        for (auto const& c : g_conns) {
            auto topics = c->getContext<Topics>();
            if (!topics) continue;
            Json::Value msg;
            msg["type"] = "snapshot";
            Json::Value da(Json::arrayValue);
            for (auto const& d : dps) {
                if (!wants(*topics, "dp", d.id)) continue;
                Json::Value o; o["id"] = d.id; o["value"] = valueToJson(d.value);
                o["quality"] = d.state; o["ts"] = Json::Int64(d.ts);
                da.append(o);
            }
            Json::Value ta(Json::arrayValue);
            for (auto const& t : tps) {
                if (!wants(*topics, "transport", t.id)) continue;
                Json::Value o; o["id"] = t.id; o["kind"] = t.kind; o["state"] = t.state;
                ta.append(o);
            }
            msg["datapoints"] = da;
            msg["transports"] = ta;
            c->send(Json::writeString(wb, msg));
        }
    });
}

} // namespace wc
