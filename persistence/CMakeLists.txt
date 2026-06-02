project(CorePersistence LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Sql Qml)
find_package(QxOrm REQUIRED)

add_library(CorePersistence SHARED
    CorePersistence_global.h
    include/core/persistence/Persistence.h
    src/Persistence.cpp
    models/Telemetry.h    models/Telemetry.cpp
    models/OperationLog.h models/OperationLog.cpp
    models/SystemLog.h    models/SystemLog.cpp
)

target_compile_definitions(CorePersistence PRIVATE COREPERSISTENCE_LIBRARY)

target_include_directories(CorePersistence
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}          # CorePersistence_global.h
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}          # "models/Foo.h" from src/
        ${QxOrm_INCLUDE_DIR}
)

target_link_libraries(CorePersistence
    PUBLIC
        Core::Core
        Qt6::Core
        Qt6::Sql
        Qt6::Qml
    PRIVATE
        QxOrm::QxOrm
)

if(MSVC)
    # QxOrm headers are not -W4 clean; keep the module at a sane warning level.
    target_compile_options(CorePersistence PRIVATE /utf-8 /bigobj)
endif()

# Live-DB smoke check: connects to the configured Postgres, writes one of each
# record type, reads them back. Built with the module; run manually.
add_executable(persistence_smoke src/smoke_main.cpp)
target_link_libraries(persistence_smoke PRIVATE CorePersistence Core::Core Qt6::Core)
