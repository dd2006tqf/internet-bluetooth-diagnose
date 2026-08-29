## ADDED Requirements

### Requirement: Unified eBPF monitor observability
The server MUST expose a common observation contract for `DnsMonitor`, `WifiPacketLossMonitor`, `HttpLatencyMonitor`, `ProcessNetProfiler`, `TcpRetransMonitor` and `BtAudioAnalyzer`. The contract MUST provide a stable monitor name, lifecycle state, availability and health snapshot without removing existing monitor-specific statistics APIs.

#### Scenario: All monitors report initial state
- **WHEN** a new monitor instance is constructed before initialization
- **THEN** its common snapshot reports an uninitialized, unavailable and unhealthy state

#### Scenario: Monitor reports attached state
- **WHEN** a monitor loads its BPF object and attaches its required probes successfully
- **THEN** its common snapshot reports attached and available state

#### Scenario: Monitor reports fallback state
- **WHEN** `BtAudioAnalyzer` cannot attach its optional eBPF hook but its D-Bus fallback remains usable
- **THEN** its snapshot reports fallback with diagnostic information and distinguishes it from fully attached health

#### Scenario: Monitor reports stopped state
- **WHEN** `stop()` completes
- **THEN** its common snapshot reports stopped and unavailable state

### Requirement: eBPF monitor performance metrics
The server MUST collect a thread-safe performance snapshot for each monitor, including attached probe count, map read count, map read error count, sample count, cumulative and average read time, and last error. The snapshot MUST be readable and resettable without changing business statistics.

#### Scenario: Successful map read updates metrics
- **WHEN** a monitor successfully reads a BPF map or statistics snapshot
- **THEN** its read and sample counters increase and its read duration is updated

#### Scenario: Failed map read updates metrics
- **WHEN** a BPF map lookup or statistics read fails
- **THEN** its error counter increases and the latest error is exposed

#### Scenario: Metrics reset
- **WHEN** a caller resets monitor metrics
- **THEN** counters and accumulated duration return to zero while lifecycle state remains unchanged

### Requirement: eBPF health query
The server MUST expose `GetEbpfMonitorHealth` through the session D-Bus service and return a deterministic JSON snapshot containing all six monitor names, lifecycle and health states, and performance metrics.

#### Scenario: Query all monitor health
- **WHEN** a connected client invokes `GetEbpfMonitorHealth`
- **THEN** the response contains entries for all six monitors

#### Scenario: Partial eBPF availability
- **WHEN** one or more monitors cannot load because the kernel or capability is unavailable
- **THEN** the response includes unavailable or fallback entries with diagnostics and still includes other monitors

#### Scenario: Invalid service context
- **WHEN** the server cannot access the monitor context
- **THEN** the method returns a D-Bus error reply
