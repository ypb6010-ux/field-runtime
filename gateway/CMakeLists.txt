# SPDX-FileCopyrightText: 2026 ypb6010-ux
# SPDX-License-Identifier: MPL-2.0

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

add_executable(field_gateway
    main.cpp
    AsioModbusTcpClient.cpp
    AsioModbusTcpServer.cpp
    AsioMqttClient.cpp
    ControlSocket.cpp
    GatewayAssembly.cpp
    GatewayJson.cpp
    Persistence.cpp
    StubTransport.cpp
)

set_target_properties(field_gateway PROPERTIES AUTOMOC OFF)
target_compile_definitions(field_gateway PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)

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
    if(NOT TARGET Boost::asio)
        find_package(boost_asio CONFIG REQUIRED)
    endif()
    if(NOT TARGET asio)
        add_library(asio INTERFACE)
        target_link_libraries(asio INTERFACE Boost::asio)
        target_compile_definitions(asio INTERFACE FIELDRUNTIME_GATEWAY_USE_BOOST_ASIO=1)
    endif()
endif()

find_package(SQLite3 REQUIRED)

target_link_libraries(field_gateway
    PRIVATE
        FieldRuntime::Base
        gateway_nanomodbus
        asio
        SQLite::SQLite3
)

target_include_directories(field_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

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
install(TARGETS field_gateway_opbox_client RUNTIME DESTINATION bin)
install(TARGETS field_gateway_control_client RUNTIME DESTINATION bin)
install(TARGETS field_gateway_mqtt_mock_broker RUNTIME DESTINATION bin)
install(FILES gateway.toml DESTINATION share/field_gateway)
