# Changelog

## Unreleased

### Changed

- Replaced the mixed `TransportEvent` contract with revisioned
  `TransportStateChanged`, `PeerSessionChanged`, and
  `SchedulerCircuitChanged` events. Applications should subscribe before
  `ICore::start()` and use the new value-snapshot APIs for initial state.
- `[[bridge]]` configurations with `mirror_count > 0` must now declare exactly
  one `mirror_policy`: `AfterPoll` or `Periodic`. `mirror_period_ms` is valid
  and required only for `Periodic`.
- Bridge mirrors now consume successful raw `PollRange` snapshots. They no
  longer reconstruct registers from decoded datapoints or require placeholder
  datapoints for undefined protocol addresses.

### Added

- Transactional `ICore::reloadConfig()` with preflight validation, rollback,
  reload lifecycle events, and `DatapointModelRebuilt` notification.
- Thread-safe `TransportStatus` and Modbus TCP server `PeerSession` snapshots.

### Runtime migration note

After a successful reload, reacquire datapoint/QML model references when
`DatapointModelRebuilt` is received. Operator-box command forwarding is closed
in the new graph until the application explicitly confirms remote-control
permission with `setServerForwardEnabled(serverId, true)`.
