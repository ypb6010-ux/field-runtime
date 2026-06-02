cmake_minimum_required(VERSION 3.21)
project(Core
    VERSION 0.1.0
    DESCRIPTION "Industrial Modbus runtime with typed datapoints and declarative routing"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(CORE_BUILD_TESTS    "Build Core unit tests"     ON)
option(CORE_BUILD_EXAMPLES "Build Core example apps"   ON)
option(CORE_BUILD_QML      "Build QML integration"     ON)
option(CORE_BUILD_LUA      "Enable Lua codec support"  OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

set(CORE_QT_COMPONENTS Core Network SerialBus SerialPort Sql)
if(CORE_BUILD_QML)
    list(APPEND CORE_QT_COMPONENTS Qml)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS ${CORE_QT_COMPONENTS})
find_package(async_simple CONFIG REQUIRED)

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
# Library target
# ---------------------------------------------------------------------------

add_library(Core SHARED)
add_library(Core::Core ALIAS Core)

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

if(MSVC)
    target_compile_options(Core PRIVATE /W4 /permissive- /utf-8)
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
    enable_testing()
    add_subdirectory(tests)
endif()

if(CORE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
