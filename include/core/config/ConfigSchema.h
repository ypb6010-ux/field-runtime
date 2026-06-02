#pragma once

#include <QList>
#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/sched/SchedulerTypes.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/transport/TransportTypes.h"

namespace core::config {

// Raw, parsed-from-TOML config records. Validation transforms these into
// concrete runtime objects (Transport, FunctionalModule, Datapoint, ...).

struct MetaConfig {
    QString project;
    QString version;
    QString generated;
    QString logLevel;   // trace|debug|info|warn|error|critical; empty = default
};

struct TransportConfig {
    QString                    id;
    transport::TransportKind   kind;
    // ─── modbus_tcp_client / opc_ua_client / mqtt_client / s7_client ────
    QString                    host;
    int                        port = 502;
    int                        slaveId = 1;
    // ─── modbus_tcp_server ──────────────────────────────────────────
    QString                    listenAddress;
    int                        listenPort = 502;
    int                        maxClients = 1;
    QList<transport::WatchRange> listenRanges;
    // ─── modbus_rtu ─────────────────────────────────────────────────
    QString                    portName;            // e.g. "COM3", "/dev/ttyUSB0"
    int                        baudRate    = 9600;
    int                        dataBits    = 8;
    int                        stopBits    = 1;
    QString                    parity      = QStringLiteral("none");  // none / even / odd
    // ─── opc_ua_client ──────────────────────────────────────────────
    QString                    endpointUrl;         // opc.tcp://host:port/path
    QString                    securityPolicy = QStringLiteral("None");
    QString                    username;
    QString                    password;
    QString                    opcuaBackend  = QStringLiteral("open62541");
    QString                    nodeIdTemplate = QStringLiteral("ns=2;s=Var_%1");
    // ─── mqtt_qt_client / mqtt_paho_client ──────────────────────────
    QString                    clientId;
    QString                    brokerUri;           // tcp://host:port or ssl://...
    QString                    topicPrefix;
    QString                    topicTemplate = QStringLiteral("reg/%1");
    int                        qos      = 1;
    bool                       cleanSession = true;
    // ─── s7_client ──────────────────────────────────────────────────
    int                        rack = 0;
    int                        slot = 1;
    // ─── common ─────────────────────────────────────────────────────
    int                        reconnectIntervalMs = 15000;
    int                        connectTimeoutMs    = 3000;
    int                        requestTimeoutMs    = 1000;
    sched::SchedulerConfig     scheduler;
};

struct CodecConfig {
    QString                    id;
    QString                    kind;       // "enum_u16" / "lua" / ...
    QVariantMap                map;        // for enum_u16
    QString                    script;     // for lua
};

struct PollRangeConfig {
    QString moduleId;
    QString transport;
    QString table;
    int     startAddress = 0;
    int     count        = 0;
    int     periodMs     = 0;
    sched::Priority priority = sched::Priority::Normal;
};

struct SinkWindowFlushConfig {
    int  debounceMs       = 20;
    int  keepaliveMs      = 0;
    bool coalesceWrites   = true;
    int  maxRetries       = 2;
};

struct SinkWindowConfig {
    QString moduleId;
    QString transport;
    QString table;
    int     startAddress = 0;
    int     size         = 0;
    sched::Priority       priority = sched::Priority::High;
    SinkWindowFlushConfig flush;
    QList<quint16>        initial;
};

struct HeartbeatConfig {
    QString moduleId;
    QString transport;
    QString table;
    int     address       = 0;
    QList<quint16> values;
    int     periodMs      = 0;
    sched::Priority priority = sched::Priority::Low;
    QString incrementer;   // "none" / "u16_inc" / "timestamp"
};

struct AckWatchConfig {
    QString  moduleId;
    QString  dp;
    QVariant expected;
    int      timeoutMs = 3000;
};

struct CommandWriteEntry {
    QString table;
    int     address = 0;
    quint16 value   = 0;
};

struct CommandConfig {
    QString moduleId;
    QString transport;
    sched::Priority priority = sched::Priority::High;
    bool    interruptable    = false;
    QString trigger;
    QList<CommandWriteEntry> writes;
};

struct PortRefConfig {
    QString  port;
    QString  table;
    int      address  = 0;
    int      bit      = -1;             // -1 = unset
    QString  wordOrder;                 // empty = default
    int      shift    = 0;
    quint64  mask     = 0xFFFFFFFFFFFFFFFFull;
    double   scale    = 1.0;
    double   offset   = 0.0;
    QString  codec;
    QString  dedupe;                    // "none" / "selflock"

    // Sink-only window reference (mutually exclusive with `port`)
    QString  window;
};

struct DatapointAckRef {
    QString  dp;
    QVariant expected;
    int      timeoutMs = 3000;
};

struct DatapointConfig {
    QString          id;
    QString          kind;    // "Status" / "Command" / "Bidirectional"
    dp::ScalarType   type     = dp::ScalarType::U16;
    PortRefConfig    source;
    PortRefConfig    sink;
    bool             hasSource = false;
    bool             hasSink   = false;
    QString          policy;   // "ContinuousMirror" / "EdgeOnce" / ...
    DatapointAckRef  ack;
    bool             hasAck    = false;
    QString          ui;
    QString          persist;
};

struct RouteConfig {
    QString name;
    QString from;       // dp id
    QString to;         // dp id
    QString policy;
};

struct PluginConfig {
    QString name;
    QString dllPath;
    QString config;
};

struct ConfigSchema {
    MetaConfig                meta;
    QList<TransportConfig>    transports;
    QList<CodecConfig>        codecs;
    QList<PollRangeConfig>    pollRanges;
    QList<SinkWindowConfig>   sinkWindows;
    QList<HeartbeatConfig>    heartbeats;
    QList<AckWatchConfig>     ackWatches;
    QList<CommandConfig>      commands;
    QList<DatapointConfig>    datapoints;
    QList<RouteConfig>        routes;
    QList<PluginConfig>       plugins;
};

} // namespace core::config
