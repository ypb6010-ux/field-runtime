<!--
SPDX-FileCopyrightText: 2026 ypb6010-ux
SPDX-License-Identifier: MPL-2.0
-->

# Third-Party Notices

FieldRuntime links against, or optionally builds with, the third-party
components listed below. Each remains under its own license; this file is
informational and does not relicense any of them.

> "Required" components are needed for a default build. "Optional"
> components are pulled in only when the matching `CORE_BUILD_*` option is
> enabled (see the README build switches).

| Component | Role | Required / Optional | License | Linkage |
|---|---|---|---|---|
| **Qt 6** (Core, Network, SerialBus, SerialPort, Sql; opt. OpcUa, Mqtt, Qml) | Protocol I/O, eventing, types, optional QML/MQTT/OPC UA | Required (Sql/SerialBus core); some modules optional | **LGPL-3.0** (open-source terms) / commercial | Dynamic |
| **async-simple** (Alibaba) | C++20 coroutine primitives (`Lazy`/`Task`) | Required | Apache-2.0 | Static |
| **tomlplusplus** | TOML configuration parsing | Required | MIT | Static / header |
| **Catch2** | Unit-test framework | Optional (`CORE_BUILD_TESTS`) | BSL-1.0 | Test only |
| **sol2** | Lua C++ binding for the Lua codec | Optional (`CORE_BUILD_LUA`) | MIT | Header |
| **Lua** (5.4) | Scripting engine for the Lua codec | Optional (`CORE_BUILD_LUA`) | MIT | Dynamic / static |
| **paho.mqtt.cpp** | Alternative MQTT transport backend | Optional (`CORE_BUILD_MQTT_PAHO`) | EPL-2.0 / EDL-1.0 (dual) | Dynamic |
| **QxOrm** | ORM for the persistence module | Optional (`CORE_BUILD_PERSISTENCE`) | **GPL-3.0** / commercial | Dynamic |

## License-hygiene notes

- **Qt 6** is used under its open-source (LGPL-3.0) terms. To keep LGPL
  compliance, Qt is linked **dynamically** and remains user-replaceable.
  Do not statically link Qt into a closed-source binary without reviewing
  the LGPL conditions (or holding a Qt commercial license).
- **QxOrm is GPL-3.0** under its open-source terms. It is confined to the
  **optional `persistence` module** and is **not** part of the core
  runtime. Distributing a closed-source product that links the persistence
  module against GPL QxOrm can impose GPL obligations on that product —
  either hold a QxOrm commercial license, replace the persistence backend,
  or run persistence as a separate process. Core (without
  `CORE_BUILD_PERSISTENCE`) does not depend on QxOrm.
- The dependency policy for the core runtime favors MIT / BSD / Apache-2.0
  / MPL-2.0 components and keeps GPL-only dependencies out of core. See the
  dependency-policy section of the MPL strategy document.

When in doubt about a specific build configuration, audit the actual
linked libraries with your toolchain and reconcile each against its
license before distribution.
