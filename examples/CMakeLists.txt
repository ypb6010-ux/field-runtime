# Examples build alongside the Core library. Each example is a small,
# self-contained executable that demonstrates one slice of the API.

add_executable(example_minimal_modbus minimal_modbus/main.cpp)
target_link_libraries(example_minimal_modbus PRIVATE Core::Core Qt6::Core)

add_executable(example_operator_box_to_plc operator_box_to_plc/main.cpp)
target_link_libraries(example_operator_box_to_plc PRIVATE Core::Core Qt6::Core)

add_executable(example_stats_dashboard stats_dashboard/main.cpp)
target_link_libraries(example_stats_dashboard PRIVATE Core::Core Qt6::Core)
