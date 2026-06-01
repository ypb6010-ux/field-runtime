# Core

> Industrial Modbus runtime: typed datapoints, declarative routing, half-duplex aware scheduling.

A C++20 / Qt6 library for building SCADA-style upper-computer software that bridges operator boxes and PLCs over Modbus TCP. Designed from the ground up around four ideas:

- **Datapoint model** — protocols are described as named logical signals, not raw register addresses. Codecs handle byte/word order, scale, mask, enum.
- **Transport + Scheduler abstraction** — half-duplex (485-to-Ethernet gateway) and full-duplex devices are both first-class, switched via configuration.
- **Functional modules** — `PollRange`, `SinkWindow`, `Heartbeat`, `AckWatch`, `Command` — every long-lived operation has a single uniform shape.
- **Declarative TOML configuration** — strict schema validation at startup, no runtime surprises.

## Status

Pre-alpha. Public APIs are sketched per the design specification; implementations land in phases.

See:
- [`doc/design/Core-Greenfield.md`](../doc/design/Core-Greenfield.md) — concept
- [`doc/design/Core-Greenfield-Spec.md`](../doc/design/Core-Greenfield-Spec.md) — full implementation specification
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
