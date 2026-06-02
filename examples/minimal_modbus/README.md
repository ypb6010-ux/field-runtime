# Example: minimal_modbus

The shortest possible Core consumer. Three datapoints, one Modbus TCP
transport, one PollRange, print every change.

## Run

1. Start any Modbus TCP simulator on `127.0.0.1:51500` (ModRSsim, ModbusPal,
   `pymodbus` server, ...). Make sure address 0..7 in Holding Registers
   exist.
2. `example_minimal_modbus minimal.toml`

Sample output:

```
21:04:33.241  temperature = 22.5
21:04:33.241  pressure    = 101
21:04:33.241  speed_rpm   = 1450.75
21:04:33.443  speed_rpm   = 1451.10
...
```

## What this demonstrates

- `ICore::create` + `loadConfig` + `start` — the canonical lifecycle
- `EventBus::subscribe<DpChanged>` — reactive consumption without polling
- `Status` datapoints with `scale` / `offset` / `wordOrder=CDAB` codecs
- Auto-reconnect on transport drop (kill / restart the simulator and watch
  Core resume polling without restarting the app)
