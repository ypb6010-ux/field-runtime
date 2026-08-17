# Sources for the Core library. As the implementation lands in phases, add
# new .cpp files here. Header-only public interfaces live under ../include.

target_sources(Core PRIVATE
    Version.cpp
    Stubs.cpp
    EventBus.cpp
    SerialScheduler.cpp
    CreditScheduler.cpp
    PriorityScheduler.cpp
    BuiltinCodecs.cpp
    CodecRegistry.cpp
    LuaCodec.cpp
    Datapoint.cpp
    DatapointRegistry.cpp
    DpTypeHelpers.cpp
    PollRange.cpp
    SinkWindow.cpp
    Heartbeat.cpp
    Command.cpp
    AckWatch.cpp
    ModuleRegistry.cpp
    PluginSystem.cpp
    ModbusTcpClientTransport.cpp
    ModbusTcpServerTransport.cpp
    ModbusRtuTransport.cpp
    OpcUaClientTransport.cpp
    MqttClientTransport.cpp
    MqttPahoTransport.cpp
    S7ClientTransport.cpp
    ConfigLoader.cpp
    ControlArbiter.cpp
    DeviceRouteManager.cpp
    Core.cpp
    Logger.cpp
    LogFilter.cpp
    DedupFilter.cpp
    Sinks.cpp
)

if(CORE_BUILD_QML)
    target_sources(Core PRIVATE
        QtDatapoint.cpp
        LogBridge.cpp
        # Headers carrying Q_OBJECT must be listed for AUTOMOC to pick them up
        # when they live outside the .cpp's directory.
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/qml/DatapointQmlBridge.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/qml/QtDatapoint.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/qml/LogBridge.h
    )
endif()
