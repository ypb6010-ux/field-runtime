cmake_minimum_required(VERSION 3.21)
project(FieldRuntime
    VERSION 2.0.0
    DESCRIPTION "Industrial field-device runtime with typed datapoints and declarative routing"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CORE_WITH_QT        "Build the Qt assembly (Core SHARED + transports/QML/tests/examples). OFF builds only the Qt-free FieldRuntimeBase + its header self-test." ON)
option(CORE_BUILD_TESTS    "Build Core unit tests"                   ON)
option(CORE_BUILD_EXAMPLES "Build Core example apps"                 ON)
option(CORE_BUILD_QML      "Build QML integration"                   ON)
option(CORE_BUILD_LUA      "Enable Lua codec support"                OFF)
option(CORE_BUILD_OPCUA    "Enable OPC UA Transport (Qt6::OpcUa)"    ON)
option(CORE_BUILD_MQTT_QT  "Enable MQTT Transport (Qt6::Mqtt)"       ON)
option(CORE_BUILD_MQTT_PAHO "Enable MQTT Transport (paho.mqtt.cpp)"  ON)
option(CORE_BUILD_PERSISTENCE "Build CorePersistence module (QxOrm)" OFF)
option(CORE_BUILD_GATEWAY  "Build the Qt-free field gateway executable" OFF)
option(CORE_BUILD_WEB_CONSOLE "Build the web_console example (Drogon)"  ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if(CORE_WITH_QT)
    set(CMAKE_AUTOMOC ON)
    set(CORE_QT_COMPONENTS Core Network SerialBus SerialPort Sql)
    if(CORE_BUILD_QML)
        list(APPEND CORE_QT_COMPONENTS Qml)
    endif()
    if(CORE_BUILD_OPCUA)
        list(APPEND CORE_QT_COMPONENTS OpcUa)
    endif()
    if(CORE_BUILD_MQTT_QT)
        list(APPEND CORE_QT_COMPONENTS Mqtt)
    endif()
    find_package(Qt6 6.8 REQUIRED COMPONENTS ${CORE_QT_COMPONENTS})
endif()
find_package(async_simple CONFIG REQUIRED)
if(CORE_WITH_QT AND CORE_BUILD_MQTT_PAHO)
    find_package(PahoMqttCpp CONFIG QUIET)
    if(NOT PahoMqttCpp_FOUND)
        message(STATUS "PahoMqttCpp not found — disabling MqttPahoTransport. "
                       "Install paho-mqttpp3 via vcpkg or point PahoMqttCpp_DIR.")
        set(CORE_BUILD_MQTT_PAHO OFF)
    endif()
endif()

find_package(tomlplusplus 3.4 QUIET)
if(NOT tomlplusplus_FOUND)
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        v3.4.0
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# ---------------------------------------------------------------------------
# FieldRuntimeBase — the Qt-free base library (types + algorithms). No Qt, no
# QModbus. This is what the without-Qt gateway (and OpenWRT/musl builds) link.
# Built unconditionally; its sources are the .cpp that carry zero Qt. Because it
# links no Qt6:: target, its TUs are compiled with NO Qt on the include path —
# so this target itself is the "base compiles without Qt" red line.
# ---------------------------------------------------------------------------

set(FIELDRUNTIME_BASE_SOURCES
    src/Version.cpp
    src/EventBus.cpp
    src/SerialScheduler.cpp
    src/CreditScheduler.cpp
    src/PriorityScheduler.cpp
    src/CodecRegistry.cpp
    src/BuiltinCodecs.cpp
    src/LuaCodec.cpp
    src/ConfigLoader.cpp
    src/Datapoint.cpp
    src/DatapointRegistry.cpp
    src/DpTypeHelpers.cpp
    src/PollRange.cpp
    src/SinkWindow.cpp
    src/Heartbeat.cpp
    src/Command.cpp
    src/AckWatch.cpp
    src/Logger.cpp
    src/LogFilter.cpp
    src/DedupFilter.cpp
    src/Sinks.cpp
    src/S7ClientTransport.cpp
)

add_library(FieldRuntimeBase STATIC ${FIELDRUNTIME_BASE_SOURCES})
add_library(FieldRuntime::Base ALIAS FieldRuntimeBase)
set_target_properties(FieldRuntimeBase PROPERTIES AUTOMOC OFF)
target_compile_definitions(FieldRuntimeBase
    PUBLIC  CORE_STATIC_BUILD
    PRIVATE CORE_BUILDING_LIBRARY)
target_include_directories(FieldRuntimeBase
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(FieldRuntimeBase
    PUBLIC  async_simple::async_simple_static
    PRIVATE tomlplusplus::tomlplusplus)
if(CORE_BUILD_LUA)
    find_package(sol2 CONFIG REQUIRED)
    find_package(Lua QUIET)
    if(NOT LUA_FOUND)
        find_path(LUA_INCLUDE_DIR NAMES lua.h)
        find_library(LUA_LIBRARIES NAMES lua lua54 lua5.4 liblua)
    endif()
    target_link_libraries(FieldRuntimeBase PRIVATE sol2::sol2 ${LUA_LIBRARIES})
    target_include_directories(FieldRuntimeBase PRIVATE ${LUA_INCLUDE_DIR})
    target_compile_definitions(FieldRuntimeBase PRIVATE CORE_HAS_LUA=1)
endif()
if(MSVC)
    target_compile_options(FieldRuntimeBase PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4251)
else()
    target_compile_options(FieldRuntimeBase PRIVATE -Wall -Wextra -Wpedantic)
endif()

# No-Qt red line: the freestanding header self-test must compile + link against
# FieldRuntimeBase with no Qt anywhere. If a base header pulls QtCore back in,
# this target stops building.
if(CORE_BUILD_TESTS)
    enable_testing()
    add_executable(fieldruntime_base_selftest tests/freestanding/qtfree_headers.cpp)
    set_target_properties(fieldruntime_base_selftest PROPERTIES AUTOMOC OFF)
    target_link_libraries(fieldruntime_base_selftest PRIVATE FieldRuntimeBase)
    add_test(
        NAME fieldruntime_base_selftest
        COMMAND fieldruntime_base_selftest)
endif()

if(CORE_BUILD_GATEWAY)
    add_subdirectory(gateway)
    # web_console example (Drogon backend + data simulation) — Qt-free, reuses
    # the gateway transports/mocks. Optional: a lean Modbus-only gateway build
    # can skip it (and the Drogon dependency).
    if(CORE_BUILD_WEB_CONSOLE)
        add_subdirectory(examples/web_console)
    endif()
endif()

# ---------------------------------------------------------------------------
# Qt assembly (gated): Core SHARED facade + Qt transports + QML + tests/examples
# ---------------------------------------------------------------------------
if(CORE_WITH_QT)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------

add_library(Core SHARED)
add_library(Core::Core ALIAS Core)
add_library(FieldRuntime::FieldRuntime ALIAS Core)

target_compile_definitions(Core PRIVATE CORE_BUILDING_LIBRARY)

target_include_directories(Core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(Core
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::SerialBus
        Qt6::SerialPort
        Qt6::Sql
        async_simple::async_simple_static
    PRIVATE
        tomlplusplus::tomlplusplus
)

if(CORE_BUILD_QML)
    target_link_libraries(Core PUBLIC Qt6::Qml)
endif()
if(CORE_BUILD_OPCUA)
    target_link_libraries(Core PUBLIC Qt6::OpcUa)
    target_compile_definitions(Core PRIVATE CORE_HAS_OPCUA=1)
endif()
if(CORE_BUILD_MQTT_QT)
    target_link_libraries(Core PUBLIC Qt6::Mqtt)
    target_compile_definitions(Core PRIVATE CORE_HAS_MQTT_QT=1)
endif()
if(CORE_BUILD_LUA)
    find_package(sol2 CONFIG REQUIRED)
    # vcpkg's Lua ships an unversioned lua.h, which CMake's FindLua can miss when
    # the vcpkg toolchain wrapper isn't active (this build dir uses CMAKE_PREFIX_PATH
    # only). Try FindLua, then fall back to locating the header/lib directly.
    find_package(Lua QUIET)
    if(NOT LUA_FOUND)
        find_path(LUA_INCLUDE_DIR NAMES lua.h)
        find_library(LUA_LIBRARIES NAMES lua lua54 lua5.4 liblua)
    endif()
    if(NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARIES)
        message(FATAL_ERROR "CORE_BUILD_LUA=ON but Lua was not found. Set "
            "LUA_INCLUDE_DIR / LUA_LIBRARIES, or configure with the vcpkg toolchain.")
    endif()
    target_link_libraries(Core PRIVATE sol2::sol2 ${LUA_LIBRARIES})
    target_include_directories(Core PRIVATE ${LUA_INCLUDE_DIR})
    target_compile_definitions(Core PRIVATE CORE_HAS_LUA=1)
endif()
if(CORE_BUILD_MQTT_PAHO)
    # vcpkg ships paho.mqtt.cpp under PahoMqttCpp::paho-mqttpp3 (shared) or
    # PahoMqttCpp::paho-mqttpp3-static, depending on triplet selection.
    if(TARGET PahoMqttCpp::paho-mqttpp3)
        target_link_libraries(Core PUBLIC PahoMqttCpp::paho-mqttpp3)
    elseif(TARGET PahoMqttCpp::paho-mqttpp3-static)
        target_link_libraries(Core PUBLIC PahoMqttCpp::paho-mqttpp3-static)
    endif()
    target_compile_definitions(Core PRIVATE CORE_HAS_MQTT_PAHO=1)
endif()

if(MSVC)
    # /EHsc is required: ConfigLoader uses try/catch around toml::parse, and
    # toml++ switches to its non-throwing parse_result API (whose table
    # conversion makes `root = toml::parse(...)` ambiguous) when C++ exceptions
    # are off. Some preset/Qt configs don't add /EHsc by default, so force it.
    #
    # /wd4251: the de-Qt migration exports value types (e.g. LogFilter) that hold
    # std:: containers as members. C4251 ("needs dll-interface") is the MSVC
    # notice for that; it is benign here because Core and every consumer build
    # with the same toolchain/runtime (/MD). Qt's own QHash/QString suppress it
    # the same way.
    target_compile_options(Core PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4251)
else()
    target_compile_options(Core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(CORE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(CORE_BUILD_PERSISTENCE)
    add_subdirectory(persistence)
endif()

if(CORE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# Core 是 SHARED 库,各产品运行时都依赖 Core.dll。windeployqt(qt_deploy_runtime_dependencies
# 的 ADDITIONAL_LIBRARIES)只扫描 Core.dll 来解析它的 Qt 依赖,并不会复制 Core.dll 本身,
# 因此必须在这里显式安装,否则 cmake --install 后产品目录里缺 Core.dll。
include(GNUInstallDirs)
install(TARGETS Core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

endif()  # CORE_WITH_QT
