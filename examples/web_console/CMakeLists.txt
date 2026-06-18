# SPDX-FileCopyrightText: 2026 ypb6010-ux
# SPDX-License-Identifier: MPL-2.0
#
# web_console example: a Drogon-based web management console for the FieldRuntime
# runtime, plus a hardware-free data-simulation service. Qt-free; reuses the
# gateway's transports/mocks. Built when CORE_BUILD_GATEWAY is ON.

# open62541 / snap7 imported targets are directory-scoped; re-find here so they
# are visible in this subdirectory (find_package is cached, so this is cheap).
find_package(open62541 CONFIG REQUIRED)
find_package(snap7 CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)

# ── W0: data simulation service (Modbus + S7 + OPC UA faces) ────────────────
add_executable(field_console_dataservice
    dataservice/main.cpp
    dataservice/SimEngine.cpp
)
set_target_properties(field_console_dataservice PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_console_dataservice PRIVATE _WIN32_WINNT=0x0601)
target_include_directories(field_console_dataservice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dataservice
    ${CMAKE_SOURCE_DIR}/gateway
    ${CMAKE_SOURCE_DIR}/gateway/third_party/nanomodbus
)
target_link_libraries(field_console_dataservice PRIVATE
    gateway_nanomodbus
    asio::asio
    open62541::open62541
    snap7::snap7
)
if(MSVC)
    target_compile_options(field_console_dataservice PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(field_console_dataservice PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS field_console_dataservice RUNTIME DESTINATION bin)

# ── W1: Drogon backend skeleton (HTTP + SQLite + static SPA) ────────────────
find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(web_console_backend
    backend/main.cpp
    backend/AuthControllers.cpp
    backend/ConfigControllers.cpp
    backend/ConfigApply.cpp
    backend/ConversionEngine.cpp
    backend/DataControllers.cpp
    backend/DocsControllers.cpp
    backend/WsControllers.cpp
    backend/RuntimeHost.cpp
)
set_target_properties(web_console_backend PROPERTIES AUTOMOC OFF)
target_compile_definitions(web_console_backend PRIVATE
    _WIN32_WINNT=0x0601
    WEB_CONSOLE_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/backend/db/schema.sql"
    WEB_CONSOLE_WWW="${CMAKE_CURRENT_SOURCE_DIR}/www"
    WEB_CONSOLE_RUNTIME_TOML="${CMAKE_CURRENT_SOURCE_DIR}/backend/runtime.toml"
    WEB_CONSOLE_OPENAPI="${CMAKE_CURRENT_SOURCE_DIR}/docs/openapi.yaml"
)
target_link_libraries(web_console_backend PRIVATE
    Drogon::Drogon
    unofficial::sqlite3::sqlite3
    field_gateway_core
)
if(MSVC)
    # htonll is provided by backend/Platform.h before any drogon header, so no
    # /Zc:twoPhase- needed (that flag breaks asio's executor templates).
    target_compile_options(web_console_backend PRIVATE /utf-8 /EHsc)
endif()

install(TARGETS web_console_backend RUNTIME DESTINATION bin)
install(DIRECTORY www/ DESTINATION share/web_console/www)
install(FILES backend/db/schema.sql DESTINATION share/web_console)
