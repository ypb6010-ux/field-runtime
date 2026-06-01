# Core

> Industrial Modbus runtime: typed datapoints, declarative routing, half-duplex aware scheduling.

A C++20 / Qt6 library for building SCADA-style upper-computer software that bridges operator boxes and PLCs over Modbus TCP. Designed from the ground up around four ideas:

- **Datapoint model** — protocols are described as named logical signals, not raw register addresses. Codecs handle byte/word order, scale, mask, enum.
- **Transport + Scheduler abstraction** — half-duplex (485-to-Ethernet gateway) and full-duplex devices are both first-class, switched via configuration.
- **Functional modules** — `PollRange`, `SinkWindow`, `Heartbeat`, `AckWatch`, `Command` — every long-lived operation has a single uniform shape.
- **Declarative TOML configuration** — strict schema validation at startup, no runtime surprises.

## Status

**Phase 2 in progress** — 109 unit tests passing (Phase 1 complete + most Phase 2 modules).

| Subsystem | State | Notes |
|-----------|-------|-------|
| EventBus | ✅ shipped | publish / subscribe / RAII Subscription; `waitFor` deferred to Phase 3 |
| SerialScheduler | ✅ shipped | priority lanes, round-robin, gap, circuit breaker |
| ModbusTcpClientTransport | ✅ shipped | wraps `QModbusTcpClient`, real TCP integration tests |
| ModbusTcpServerTransport | ✅ shipped | publishes `ServerWriteEvent` on dataWritten |
| Datapoint / DatapointRegistry | ✅ shipped | QObject + `Q_PROPERTY(value)` for direct QML binding |
| BuiltinScalarCodec | ✅ shipped | all 11 ScalarTypes × 4 WordOrders, scale/offset/mask/shift |
| EnumU16Codec / CodecRegistry | ✅ shipped | |
| PollRange | ✅ shipped | per-tick algorithm + datapoint bindings (`pollOnce()`) |
| SinkWindow | ✅ shipped | debounce / keepAlive / forceFlush / coalesce |
| Heartbeat / Command / AckWatch | ✅ shipped | synchronous AckWatch; coroutine variant Phase 3 |
| ConfigLoader (TOML) | 🟡 partial | 5 of 18 validation rules; sink_window / heartbeat / ack_watch / command sections pending |
| ICore facade | ✅ shipped | wires Transport / Codec / Datapoint / PollRange from TOML, real end-to-end against `QModbusTcpServer` |
| QTimer autopilot | ⏳ pending | `ModuleRegistry.startAll()` currently no-op; modules driven via test hooks |
| Transport reconnect | ⏳ pending | manual `connect()` only |
| CreditScheduler / PriorityScheduler | ⏳ pending | Phase 3 |
| LuaCodec | ⏳ pending | Phase 3 (sol2 sandbox) |
| Plugin / Database integration | ⏳ pending | Phase 3+ |

See the [implementation specification](../doc/design/Core-Greenfield-Spec.md) for the full progress table and ~150-item checklist.

References:
- [`doc/design/Core-Greenfield.md`](../doc/design/Core-Greenfield.md) — concept
- [`doc/design/Core-Greenfield-Spec.md`](../doc/design/Core-Greenfield-Spec.md) — full implementation specification + live progress table
- [`doc/design/Core-Routing.md`](../doc/design/Core-Routing.md) — alternative incremental-compatible design (not used here)

## Build

This subproject is self-contained and can be built without the rest of the parent repository.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires:

| Component | Minimum |
|-----------|---------|
| C++ standard | C++23 (`std::expected`) |
| Compiler | MSVC 19.36+ / GCC 13 / Clang 16 |
| Qt | 6.8 |
| CMake | 3.21 |

Required dependencies (declared in `vcpkg.json`):

- [async_simple](https://github.com/alibaba/async_simple) — coroutine `Lazy<T>` plus `collectAll`/`collectAllPara` helpers.

Optional dependencies (resolved during their corresponding implementation phase):

- [tomlplusplus](https://github.com/marzer/tomlplusplus) — configuration parser
- [Catch2](https://github.com/catchorg/Catch2) v3 — unit tests
- [Lua](https://www.lua.org/) 5.4 + [sol2](https://github.com/ThePhD/sol2) — script codecs

When building inside this repository, vcpkg manifest mode picks up
`vcpkg.json` automatically:

```bash
cmake -S core -B core/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DQt6_DIR=$env:QT6_DIR
```

## License

To be decided before public release. See [LICENSE](LICENSE).
