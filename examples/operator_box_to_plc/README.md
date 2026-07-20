# Example: operator_box_to_plc

A segment-level bidirectional Modbus bridge. Core acts as a **Modbus TCP server** facing
operator boxes on port 5020 and a **Modbus TCP client** to the real PLC on
port 51500. Operator-box HR[0..3] writes are batched to the same PLC addresses;
every successful PLC HR[50..53] poll mirrors that raw register image back to
the server immediately.

```
┌────────────┐    Modbus TCP     ┌──────────┐    Modbus TCP     ┌──────────┐
│ operator   │ ───── write ────► │  Core    │ ──── writeBatch ──►│  PLC     │
│ box        │                   │  bridge  │                    │ simulator│
└────────────┘                   └──────────┘                    └──────────┘
```

## Run

1. Start a Modbus TCP simulator listening on `127.0.0.1:51500` (this is the
   PLC). Make sure HR[0..3] and HR[50..53] exist.
2. `example_operator_box_to_plc bridge.toml`
3. From any other Modbus client (modpoll, ModRSsim master, ...) connect to
   `127.0.0.1:5020` and write `0x1234` into HR[0]. The PLC simulator's HR[0]
   should now read `0x1234`. Reading box HR[50..53] returns the latest complete
   raw PLC poll image.

## What this demonstrates

- `ModbusTcpServerTransport` listening on a custom port
- Bridge-owned `SinkWindow` command batching
- `bus::ServerWriteEvent` subscription showing operator activity
- `TransportStateChanged` plus per-client `PeerSessionChanged` events
- `bus::SchedulerStatsEvent` periodic publishing — queue depth, inflight,
  p99 latency
- `mirror_policy = "AfterPoll"` without placeholder datapoints or value re-encoding
