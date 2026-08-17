// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/core_global.h"
#include "core/control/ControlTypes.h"
#include "core/sched/SchedulerTypes.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/dp/Value.h"
#include "core/transport/TransportTypes.h"

namespace core::config {

// Raw, parsed-from-TOML config records. Validation transforms these into
// concrete runtime objects (Transport, FunctionalModule, Datapoint, ...).
//
// This is the Qt-free configuration IR: a pure-std representation shared by the
// Qt HMI assembly (Core), the without-Qt gateway, and a future Web 组态 backend.
// Keep it free of QtCore types.

struct MetaConfig {
    std::string project;
    std::string version;
    std::string generated;
    std::string logLevel;   // trace|debug|info|warn|error|critical; empty = default
};

struct TransportConfig {
    std::string                id;
    transport::TransportKind   kind;
    // ─── modbus_tcp_client / opc_ua_client / mqtt_client / s7_client ────
    std::string                host;
    int                        port = 502;
    int                        slaveId = 1;
    // ─── modbus_tcp_server ──────────────────────────────────────────
    std::string                listenAddress;
    int                        listenPort = 502;
    int                        maxClients = 1;
    std::vector<transport::WatchRange> listenRanges;
    // ─── modbus_rtu ─────────────────────────────────────────────────
    std::string                portName;            // e.g. "COM3", "/dev/ttyUSB0"
    int                        baudRate    = 9600;
    int                        dataBits    = 8;
    int                        stopBits    = 1;
    std::string                parity      = "none";  // none / even / odd
    // ─── opc_ua_client ──────────────────────────────────────────────
    std::string                endpointUrl;         // opc.tcp://host:port/path
    std::string                securityPolicy = "None";
    std::string                username;
    std::string                password;
    std::string                opcuaBackend  = "open62541";
    std::string                nodeIdTemplate = "ns=2;s=Var_%1";
    // ─── mqtt_qt_client / mqtt_paho_client ──────────────────────────
    std::string                clientId;
    std::string                brokerUri;           // tcp://host:port or ssl://...
    std::string                topicPrefix;
    std::string                topicTemplate = "reg/%1";
    int                        qos      = 1;
    bool                       cleanSession = true;
    // ─── s7_client ──────────────────────────────────────────────────
    int                        rack = 0;
    int                        slot = 1;
    int                        s7Db = 1;     // default DB number for HR-mapped reads/writes
    // ─── common ─────────────────────────────────────────────────────
    int                        reconnectIntervalMs = 15000;
    int                        connectTimeoutMs    = 3000;
    int                        requestTimeoutMs    = 1000;
    sched::SchedulerConfig     scheduler;
};

struct CodecConfig {
    std::string                          id;
    std::string                          kind;    // "enum_u16" / "lua" / ...
    std::map<std::string, dp::Value>     map;     // for enum_u16 (key = register value)
    std::string                          script;  // for lua
    std::string                          arg;     // for lua: opaque selector passed as ctx.arg
};

struct PollRangeConfig {
    std::string moduleId;
    std::string transport;
    std::string table;
    int     startAddress = 0;
    int     count        = 0;
    int     periodMs     = 0;
    sched::Priority priority = sched::Priority::Normal;
};

struct SinkWindowFlushConfig {
    int  debounceMs       = 20;
    int  keepaliveMs      = 0;
    bool coalesceWrites   = true;
    int  maxRetries       = 0;
};

struct SinkWindowConfig {
    std::string moduleId;
    std::string transport;
    std::string table;
    int     startAddress = 0;
    int     size         = 0;
    sched::Priority       priority = sched::Priority::High;
    SinkWindowFlushConfig flush;
    core::RegisterWords        initial;
};

struct HeartbeatConfig {
    std::string moduleId;
    std::string transport;
    std::string table;
    int     address       = 0;
    core::RegisterWords values;
    int     periodMs      = 0;
    sched::Priority priority = sched::Priority::Low;
    std::string incrementer;   // "none" / "u16_inc" / "timestamp"
};

struct AckWatchConfig {
    std::string  moduleId;
    std::string  dp;
    dp::Value    expected;
    int          timeoutMs = 3000;
};

struct CommandWriteEntry {
    std::string table;
    int         address = 0;
    uint16_t    value   = 0;
};

struct CommandConfig {
    std::string moduleId;
    std::string transport;
    sched::Priority priority = sched::Priority::High;
    bool    interruptable    = false;
    std::string trigger;
    std::vector<CommandWriteEntry> writes;
};

struct PortRefConfig {
    std::string  port;
    std::string  table;
    int          address  = 0;
    int          bit      = -1;             // -1 = unset
    std::string  wordOrder;                 // empty = default
    int          shift    = 0;
    uint64_t     mask     = 0xFFFFFFFFFFFFFFFFull;
    double       scale    = 1.0;
    double       offset   = 0.0;
    std::string  codec;
    std::string  dedupe;                    // "none" / "selflock"

    // Sink-only window reference (mutually exclusive with `port`)
    std::string  window;
};

struct DatapointAckRef {
    std::string  dp;
    dp::Value    expected;
    int          timeoutMs = 3000;
};

struct DatapointConfig {
    std::string      id;
    std::string      kind;    // "Status" / "Command" / "Bidirectional"
    dp::ScalarType   type     = dp::ScalarType::U16;
    PortRefConfig    source;
    PortRefConfig    sink;
    bool             hasSource = false;
    bool             hasSink   = false;
    std::string      policy;   // "ContinuousMirror" / "EdgeOnce" / ...
    DatapointAckRef  ack;
    bool             hasAck    = false;
    std::string      ui;
    std::string      persist;
    // Disconnect policy: "reset" (default) zeros the datapoint to disconnectValue
    // when its source transport drops; "hold" keeps the last value. Either way
    // quality goes Error so HMI/consumers never read a stale value as live.
    std::string      onDisconnect    = "reset";
    double           disconnectValue = 0.0;
};

struct RouteConfig {
    std::string name;
    std::string from;       // dp id
    std::string to;         // dp id
    std::string policy;
};

struct PluginConfig {
    std::string name;
    std::string dllPath;
    std::string config;
};

// A driver is a preinstalled in-process adapter. `library` points to the
// adapter module selected from the deployment whitelist; `config` is opaque to
// FieldRuntime and is handed to that adapter at creation time.
struct DriverConfig {
    std::string id;
    std::string library;
    std::string config;
    bool enabled = true;
};

struct ActorConfig {
    std::string id;
    std::string channel;
    std::string clientId;
    std::string sourceAddress;
    std::string role;
    int priority = 0;
    bool enabled = true;
};

struct DeviceConfig {
    std::string id;
    std::string name;
    std::string driverId;
};

struct DeviceRouteConfig {
    std::string id;
    std::string deviceId;
    std::string protocol;
    std::string transportId;
    std::string driverId;
    bool writable = true;
    bool active = false;
};

struct ControlTargetConfig {
    std::string id;
    std::string deviceId;
    std::string routeId;
    control::ControlAddress address;
};

struct ControlPolicyConfig {
    std::string id;
    std::string targetId;
    control::PolicyMode mode = control::PolicyMode::Open;
    int leaseMs = 0;
    int minPriority = 0;
};

enum class BridgeMirrorPolicy {
    AfterPoll,
    Periodic,
};

// 整段桥接(替代旧 ModbusServer 中继):把操作箱连的 server transport 与 PLC client
// transport 双向桥接 —— 写区 [write_start, write_start+write_count) 的 server 写转发到
// PLC;读区 [mirror_start, mirror_start+mirror_count) 的 PLC 原始轮询快照按指定策略
// 镜像回 server 寄存器供操作箱读取。server 地址 = PLC 地址 + offset。
struct BridgeConfig {
    std::string server;
    std::string plc;
    int     offset         = 0;
    int     writeStart     = 0;
    int     writeCount     = 0;
    int     mirrorStart    = 0;
    int     mirrorCount    = 0;
    BridgeMirrorPolicy mirrorPolicy = BridgeMirrorPolicy::AfterPoll;
    int     mirrorPeriodMs = 0;   // required only for Periodic
};

struct ConfigSchema {
    MetaConfig                     meta;
    std::vector<TransportConfig>   transports;
    std::vector<CodecConfig>       codecs;
    std::vector<PollRangeConfig>   pollRanges;
    std::vector<SinkWindowConfig>  sinkWindows;
    std::vector<HeartbeatConfig>   heartbeats;
    std::vector<AckWatchConfig>    ackWatches;
    std::vector<CommandConfig>     commands;
    std::vector<DatapointConfig>   datapoints;
    std::vector<RouteConfig>       routes;
    std::vector<BridgeConfig>      bridges;
    std::vector<PluginConfig>      plugins;
    std::vector<DriverConfig>      drivers;
    std::vector<ActorConfig>       actors;
    std::vector<DeviceConfig>      devices;
    std::vector<DeviceRouteConfig> deviceRoutes;
    std::vector<ControlTargetConfig> controlTargets;
    std::vector<ControlPolicyConfig> controlPolicies;
};

} // namespace core::config
