// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// Minimal in-process OPC UA server for the gateway OPC UA self-test. Exposes a
// handful of read-only scalar nodes (ns=2;s=Var_<addr>) that the gateway's
// AsioOpcUaClient polls through its node_id_template "ns=2;s=Var_%1".
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/types.h>

namespace {

volatile UA_Boolean g_running = true;

void onSignal(int) { g_running = false; }

// All wrappers below are non-owning (point at `name` / `value`, which outlive
// the call); UA_Server_addVariableNode deep-copies them, so nothing here is
// heap-allocated and nothing needs clearing.
void addInt32(UA_Server* server, UA_UInt16 ns, char const* name, UA_Int32 value) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_INT32]);
    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));

    UA_NodeId nodeId = UA_NODEID_STRING(ns, const_cast<char*>(name));
    UA_QualifiedName qn = UA_QUALIFIEDNAME(ns, const_cast<char*>(name));
    UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentRef = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_NodeId typeDef = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);

    UA_Server_addVariableNode(server, nodeId, parent, parentRef, qn, typeDef,
                              attr, nullptr, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    auto const port = UA_UInt16(argc > 1 ? std::stoi(argv[1]) : 4840);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    UA_Server* server = UA_Server_new();
    UA_ServerConfig_setMinimal(UA_Server_getConfig(server), port, nullptr);

    // Register application namespace -> index 2, matching the gateway's
    // node_id_template "ns=2;s=Var_%1". Namespace 2 does not exist by default.
    UA_UInt16 const ns = UA_Server_addNamespace(server, "http://fieldruntime/gateway");

    // Contiguous nodes so a single poll range [0,3) covers them all:
    //   Var_0 = 230  (temperature, scale 0.1 -> 23.0)
    //   Var_1 = 1450 (speed)
    //   Var_2 = 2    (run_state -> enum_u16 "fault")
    addInt32(server, ns, "Var_0", 230);
    addInt32(server, ns, "Var_1", 1450);
    addInt32(server, ns, "Var_2", 2);

    std::cout << "mock OPC UA server listening on opc.tcp://127.0.0.1:" << port
              << std::endl;

    UA_StatusCode rc = UA_Server_run(server, &g_running);
    UA_Server_delete(server);
    return rc == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}
