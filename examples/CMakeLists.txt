# Examples build alongside the Core library. Each example is a small,
# self-contained executable that demonstrates one slice of the API.

add_executable(example_minimal_modbus minimal_modbus/main.cpp)
target_link_libraries(example_minimal_modbus PRIVATE Core::Core Qt6::Core)

add_executable(example_operator_box_to_plc operator_box_to_plc/main.cpp)
target_link_libraries(example_operator_box_to_plc PRIVATE Core::Core Qt6::Core)

add_executable(example_stats_dashboard stats_dashboard/main.cpp)
target_link_libraries(example_stats_dashboard PRIVATE Core::Core Qt6::Core)

# ── qml_dashboard: QML showcase for the logging + persistence modules ──────
if(CORE_BUILD_QML)
    find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)

    qt_add_executable(example_qml_dashboard
        qml_dashboard/main.cpp
        qml_dashboard/DemoController.cpp
        qml_dashboard/DemoController.h
        qml_dashboard/SimulatedPlc.cpp
        qml_dashboard/SimulatedPlc.h
        qml_dashboard/UiLogSink.h
        qml_dashboard/qml.qrc
    )
    set_target_properties(example_qml_dashboard PROPERTIES
        AUTOMOC ON
        AUTORCC ON)
    target_include_directories(example_qml_dashboard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/qml_dashboard)
    target_compile_definitions(example_qml_dashboard PRIVATE
        DASHBOARD_TOML="${CMAKE_CURRENT_SOURCE_DIR}/qml_dashboard/dashboard.toml")
    target_link_libraries(example_qml_dashboard PRIVATE
        Core::Core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::SerialBus)

    if(CORE_BUILD_PERSISTENCE)
        target_link_libraries(example_qml_dashboard PRIVATE CorePersistence)
        target_compile_definitions(example_qml_dashboard PRIVATE DASHBOARD_HAS_PERSISTENCE=1)
    endif()
endif()
