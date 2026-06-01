# Sources for the Core library. As the implementation lands in phases, add
# new .cpp files here. Header-only public interfaces live under ../include.

target_sources(Core PRIVATE
    Version.cpp
    Stubs.cpp
    EventBus.cpp
    SerialScheduler.cpp
    BuiltinCodecs.cpp
    CodecRegistry.cpp
    Datapoint.cpp
    DatapointRegistry.cpp
    PollRange.cpp
    # Headers carrying Q_OBJECT must be listed for AUTOMOC to pick them up
    # when they live outside the .cpp's directory.
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/bus/EventBus.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/dp/Datapoint.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/qml/DatapointQmlBridge.h
)
