// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
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
    QString                    arg;        // for lua: opaque selector passed as ctx.arg
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
    QString incrementer;   // currently only "none" is supported
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

    // Sink-only SinkWindow module reference; `port` names its transport.
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
    QString          policy;   // optional "ContinuousMirror"
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
};

// 整段桥接(替代旧 ModbusServer 中继):把操作箱连的 server transport 与 PLC client
// transport 双向桥接 —— 写区 [write_start, write_start+write_count) 的 server 写转发到
// PLC;读区 [mirror_start, mirror_start+mirror_count) 的 PLC 数据周期镜像回 server 寄存器
// 供操作箱读取。server 地址 = PLC 地址 + offset。
struct BridgeConfig {
    QString server;
    QString plc;
    int     offset         = 0;
    int     writeStart     = 0;
    int     writeCount     = 0;
    int     mirrorStart    = 0;
    int     mirrorCount    = 0;
    int     mirrorPeriodMs = 100;
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
    QList<BridgeConfig>       bridges;
    QList<PluginConfig>       plugins;
};

} // namespace core::config
