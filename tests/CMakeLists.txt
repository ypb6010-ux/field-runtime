find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
include(Catch)

# Single test binary aggregating every *.cpp under tests/. Catch2's main
# discovery registers each TEST_CASE as a separate ctest entry.
add_executable(CoreTests
    TestMain.cpp
    SmokeTest.cpp
    unit/EventBusTests.cpp
    unit/SerialSchedulerTests.cpp
    unit/CodecTests.cpp
    unit/DatapointTests.cpp
    unit/PollRangeTests.cpp
    unit/SinkWindowTests.cpp
    unit/HeartbeatTests.cpp
    unit/CommandTests.cpp
    unit/AckWatchTests.cpp
    unit/ModbusTcpClientTransportTests.cpp
    unit/ModbusTcpServerTransportTests.cpp
    unit/ConfigLoaderTests.cpp
    unit/CoreEndToEndTests.cpp
    unit/ModuleRegistryTickTests.cpp
    unit/TransportReconnectTests.cpp
    unit/CreditPrioritySchedulerTests.cpp
    unit/AsyncSchedulerTests.cpp
    unit/ModbusRtuTransportTests.cpp
    unit/OpcUaTransportTests.cpp
    unit/MqttTransportTests.cpp
    unit/LoggerTests.cpp
    unit/LuaCodecTests.cpp
    unit/PluginSystemTests.cpp
)

# Point the Lua codec test at the real shipped script.
target_compile_definitions(CoreTests PRIVATE
    CORE_LUA_BCD_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/../data/codec/bcd_datetime.lua")

find_package(Qt6 REQUIRED COMPONENTS Test)

target_link_libraries(CoreTests
    PRIVATE
        Core::Core
        Catch2::Catch2
        Qt6::Test
        Qt6::SerialBus
)

target_include_directories(CoreTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

catch_discover_tests(CoreTests
    DL_PATHS
        $<TARGET_FILE_DIR:Core>
        $<TARGET_FILE_DIR:Qt6::Core>
)
