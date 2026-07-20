# Example: stats_dashboard

Demonstrates real-time observability via `SchedulerStatsEvent` +
`TransportStateChanged`. Output is one line per transport per second showing
queue depth, in-flight requests, p50/p99 latency, total throughput,
and circuit-breaker state.

## Run

1. Optionally start one or two Modbus TCP simulators at `127.0.0.1:51500`
   and `127.0.0.1:51501`. Without them, both transports stay in
   reconnecting / disconnected and the dashboard shows `×` markers,
   demonstrating the auto-reconnect heartbeat.
2. `example_stats_dashboard dashboard.toml`

Sample output:

```
[plc_a] ● q=0  inflight=1  p50=8ms   p99=22ms  done=42  fail=0  circuit=0
[plc_b] ● q=2  inflight=4  p50=11ms  p99=38ms  done=121 fail=0  circuit=0
[event] plc_a → Disconnected
[plc_a] × q=0  inflight=0  p50=8ms   p99=22ms  done=42  fail=3  circuit=0
[event] plc_a → Connected
[plc_a] ● q=0  inflight=1  p50=9ms   p99=24ms  done=45  fail=3  circuit=0
```

## What this demonstrates

- Two transports with different scheduler kinds (`serial` vs `credit`)
- `SchedulerStatsEvent` shape — pipe directly to Prometheus / Grafana
  or a QML diagnostics page
- immutable `TransportStatus` snapshots and revisioned
  `TransportStateChanged` transitions as a connectivity heartbeat
- Live recovery from server-side disconnects without restarting the app
