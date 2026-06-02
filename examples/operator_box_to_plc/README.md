# Example: operator_box_to_plc

A bidirectional Modbus bridge. Core acts as a **Modbus TCP server** facing
operator boxes on port 5020 and a **Modbus TCP client** to the real PLC on
port 51500. Operator-box writes land in a `SinkWindow`, which batches them
out to the PLC every 20 ms (configurable).

```
┌────────────┐    Modbus TCP     ┌──────────┐    Modbus TCP     ┌──────────┐
│ operator   │ ───── write ────► │  Core    │ ──── writeBatch ──►│  PLC     │
│ box        │                   │  bridge  │                    │ simulator│
└────────────┘                   └──────────┘                    └──────────┘
```

## Run

1. Start a Modbus TCP simulator listening on `127.0.0.1:51500` (this is the
   PLC). Make sure HR[100..103] exist.
2. `example_operator_box_to_plc bridge.toml`
3. From any other Modbus client (modpoll, ModRSsim master, ...) connect to
   `127.0.0.1:5020` and write `0x1234` into HR[0]. The PLC simulator's
   HR[100] should now read `0x1234`.

## What this demonstrates

- `ModbusTcpServerTransport` listening on a custom port
- `SinkWindow` debounced batched writes
- `bus::ServerWriteEvent` subscription showing operator activity
- `bus::SchedulerStatsEvent` periodic publishing — queue depth, inflight,
  p99 latency
- Route table for transparent server-side → PLC-side mirroring
