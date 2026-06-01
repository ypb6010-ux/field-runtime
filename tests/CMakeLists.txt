add_executable(CoreSmokeTest
    SmokeTest.cpp
)
target_link_libraries(CoreSmokeTest PRIVATE Core::Core)
add_test(NAME CoreSmokeTest COMMAND CoreSmokeTest)

# Future test layout placeholders (each will become its own executable):
#   unit/       — fine-grained unit tests per subsystem
#   integration/— end-to-end scenarios from design doc §9
#   mocks/      — shared MockTransport / MockScheduler / MockEventBus
