# SPDX-FileCopyrightText: 2026 ypb6010-ux
# SPDX-License-Identifier: MPL-2.0

# Optional southbound protocols. Off lets a lean build (e.g. Modbus-only on
# Linux with apt deps) skip the heavy open62541/snap7 dependencies.
option(GATEWAY_ENABLE_OPCUA "Build OPC UA southbound (open62541)" ON)
option(GATEWAY_ENABLE_S7 "Build S7 southbound (snap7)" ON)

add_library(gateway_nanomodbus STATIC
    third_party/nanomodbus/nanomodbus.cpp
)
set_target_properties(gateway_nanomodbus PROPERTIES AUTOMOC OFF)
target_include_directories(gateway_nanomodbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanomodbus
)
if(MSVC)
    target_compile_options(gateway_nanomodbus PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(gateway_nanomodbus PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(asio CONFIG QUIET)
if(TARGET asio::asio)
    if(NOT TARGET asio)
        add_library(asio INTERFACE)
        target_link_libraries(asio INTERFACE asio::asio)
    endif()
elseif(TARGET unofficial::asio::asio)
    if(NOT TARGET asio)
        add_library(asio INTERFACE)
        target_link_libraries(asio INTERFACE unofficial::asio::asio)
    endif()
else()
    set(_gateway_vcpkg_prefixes)
    if(DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED VCPKG_TARGET_TRIPLET)
        get_filename_component(_gateway_vcpkg_toolchain_dir "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
        get_filename_component(_gateway_vcpkg_root "${_gateway_vcpkg_toolchain_dir}/../.." ABSOLUTE)
        list(APPEND _gateway_vcpkg_prefixes
            "${_gateway_vcpkg_root}/installed/${VCPKG_TARGET_TRIPLET}")
    endif()
    find_package(boost_asio CONFIG QUIET PATHS ${_gateway_vcpkg_prefixes})
    if(TARGET Boost::asio)
        if(NOT TARGET asio)
            add_library(asio INTERFACE)
            target_link_libraries(asio INTERFACE Boost::asio)
            target_compile_definitions(asio INTERFACE FIELDRUNTIME_GATEWAY_USE_BOOST_ASIO=1)
        endif()
    else()
        # Standalone asio headers (e.g. apt libasio-dev), no CMake config.
        find_path(ASIO_INCLUDE_DIR NAMES asio.hpp)
        if(NOT ASIO_INCLUDE_DIR)
            message(FATAL_ERROR
                "asio not found: install vcpkg asio/boost or apt libasio-dev")
        endif()
        find_package(Threads REQUIRED)
        if(NOT TARGET asio)
            add_library(asio INTERFACE)
            target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
            target_link_libraries(asio INTERFACE Threads::Threads)
        endif()
    endif()
endif()

find_package(SQLite3 REQUIRED)
if(GATEWAY_ENABLE_OPCUA)
    find_package(open62541 CONFIG REQUIRED)
endif()
if(GATEWAY_ENABLE_S7)
    find_package(snap7 CONFIG REQUIRED)
endif()

# Promote imported targets to GLOBAL so a sibling subdirectory
# (examples/web_console) can consume them transitively via field_gateway_core.
foreach(_t SQLite::SQLite3 open62541::open62541 snap7::snap7)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
endforeach()

# ── Reusable runtime library: transports + assembly + control/persistence
#    (everything the gateway exe is, minus main.cpp). The web_console backend
#    embeds this to host a FieldRuntime runtime in-process. ──
set(_gw_core_sources
    AsioModbusRtuClient.cpp
    AsioModbusTcpClient.cpp
    AsioModbusTcpServer.cpp
    AsioMqttClient.cpp
    ControlSocket.cpp
    GatewayAssembly.cpp
    GatewayJson.cpp
    Persistence.cpp
    StubTransport.cpp
)
if(GATEWAY_ENABLE_OPCUA)
    list(APPEND _gw_core_sources AsioOpcUaClient.cpp)
endif()
if(GATEWAY_ENABLE_S7)
    list(APPEND _gw_core_sources AsioS7Client.cpp)
endif()
add_library(field_gateway_core STATIC ${_gw_core_sources})
set_target_properties(field_gateway_core PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_core PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
target_include_directories(field_gateway_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(field_gateway_core PUBLIC
    FieldRuntime::Base
    gateway_nanomodbus
    asio
    SQLite::SQLite3
)
if(GATEWAY_ENABLE_OPCUA)
    target_link_libraries(field_gateway_core PUBLIC open62541::open62541)
    target_compile_definitions(field_gateway_core PRIVATE FIELDRUNTIME_GATEWAY_HAS_OPCUA=1)
endif()
if(GATEWAY_ENABLE_S7)
    target_link_libraries(field_gateway_core PUBLIC snap7::snap7)
    target_compile_definitions(field_gateway_core PRIVATE FIELDRUNTIME_GATEWAY_HAS_S7=1)
endif()
if(MSVC)
    target_compile_options(field_gateway_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway main.cpp)
set_target_properties(field_gateway PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(field_gateway PRIVATE field_gateway_core)
if(MSVC)
    target_compile_options(field_gateway PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway_mock_server
    MockModbusTcpServer.cpp
)
set_target_properties(field_gateway_mock_server PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_mock_server PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(field_gateway_mock_server
    PRIVATE
        gateway_nanomodbus
        asio
)
target_include_directories(field_gateway_mock_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(field_gateway_mock_server PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_mock_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GATEWAY_ENABLE_OPCUA)
    add_executable(field_gateway_opcua_mock_server
        OpcUaMockServer.cpp
    )
    set_target_properties(field_gateway_opcua_mock_server PROPERTIES AUTOMOC OFF)
    target_compile_definitions(field_gateway_opcua_mock_server PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(field_gateway_opcua_mock_server
        PRIVATE
            open62541::open62541
    )
    if(MSVC)
        target_compile_options(field_gateway_opcua_mock_server PRIVATE /W4 /permissive- /utf-8 /EHsc)
    else()
        target_compile_options(field_gateway_opcua_mock_server PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(GATEWAY_ENABLE_S7)
    add_executable(field_gateway_s7_mock_server
        S7MockServer.cpp
    )
    set_target_properties(field_gateway_s7_mock_server PROPERTIES AUTOMOC OFF)
    target_compile_definitions(field_gateway_s7_mock_server PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(field_gateway_s7_mock_server PRIVATE snap7::snap7)
    if(MSVC)
        target_compile_options(field_gateway_s7_mock_server PRIVATE /W4 /permissive- /utf-8 /EHsc)
    else()
        target_compile_options(field_gateway_s7_mock_server PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

add_executable(field_gateway_rtu_selftest
    RtuFramingSelfTest.cpp
)
set_target_properties(field_gateway_rtu_selftest PROPERTIES AUTOMOC OFF)
target_link_libraries(field_gateway_rtu_selftest PRIVATE gateway_nanomodbus)
if(MSVC)
    target_compile_options(field_gateway_rtu_selftest PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_rtu_selftest PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway_rtu_mock_slave
    RtuMockSlave.cpp
)
set_target_properties(field_gateway_rtu_mock_slave PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_rtu_mock_slave PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(field_gateway_rtu_mock_slave
    PRIVATE
        gateway_nanomodbus
        asio
)
target_include_directories(field_gateway_rtu_mock_slave
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(field_gateway_rtu_mock_slave PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_rtu_mock_slave PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway_opbox_client
    OpboxClient.cpp
)
set_target_properties(field_gateway_opbox_client PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_opbox_client PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(field_gateway_opbox_client
    PRIVATE
        gateway_nanomodbus
        asio
)
target_include_directories(field_gateway_opbox_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(field_gateway_opbox_client PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_opbox_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway_control_client
    ControlClient.cpp
)
set_target_properties(field_gateway_control_client PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_control_client PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(field_gateway_control_client
    PRIVATE
        asio
)
target_include_directories(field_gateway_control_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(field_gateway_control_client PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_control_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(field_gateway_mqtt_mock_broker
    MqttMockBroker.cpp
)
set_target_properties(field_gateway_mqtt_mock_broker PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway_mqtt_mock_broker PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(field_gateway_mqtt_mock_broker
    PRIVATE
        asio
)
target_include_directories(field_gateway_mqtt_mock_broker
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(field_gateway_mqtt_mock_broker PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_gateway_mqtt_mock_broker PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS field_gateway RUNTIME DESTINATION bin)
install(TARGETS field_gateway_mock_server RUNTIME DESTINATION bin)
if(GATEWAY_ENABLE_OPCUA)
    install(TARGETS field_gateway_opcua_mock_server RUNTIME DESTINATION bin)
endif()
if(GATEWAY_ENABLE_S7)
    install(TARGETS field_gateway_s7_mock_server RUNTIME DESTINATION bin)
endif()
install(TARGETS field_gateway_rtu_mock_slave RUNTIME DESTINATION bin)
install(TARGETS field_gateway_opbox_client RUNTIME DESTINATION bin)
install(TARGETS field_gateway_control_client RUNTIME DESTINATION bin)
install(TARGETS field_gateway_mqtt_mock_broker RUNTIME DESTINATION bin)
install(FILES gateway.toml DESTINATION share/field_gateway)
