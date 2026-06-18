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
