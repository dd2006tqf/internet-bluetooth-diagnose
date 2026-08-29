## 1. Shared contract and metrics

- [ ] 1 Add common monitor states, health snapshots, performance metrics and thread-safe metric tracking
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `All monitors report initial state`
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF monitor performance metrics` | `Successful map read updates metrics`
  - Verify: `build`, `test`
- [ ] 2 Remove the unused ownership-bearing `EbpfMonitorBase` or reduce it to a non-owning metrics utility
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports stopped state`
  - Verify: `build`

## 2. Six monitor adapters

- [ ] 3 Adapt `DnsMonitor`, `WifiPacketLossMonitor` and `TcpRetransMonitor` to expose the common contract and update state and metrics
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports attached state`
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF monitor performance metrics` | `Failed map read updates metrics`
  - Verify: `build`, `test`
- [ ] 4 Adapt `HttpLatencyMonitor`, `ProcessNetProfiler` and `BtAudioAnalyzer`, preserving attached/fallback/error semantics
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports fallback state`
  - Verify: `build`, `test`
- [ ] 5 Add focused tests for six-monitor states, fallback, read metrics and reset
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF monitor performance metrics` | `Metrics reset`
  - Verify: `test`

## 3. TcpRetransMonitor production integration

- [ ] 6 Add `TcpRetransMonitor` ownership, startup, shutdown and map-reading consumer for `tcp_retransmit.bpf.c` without replacing `TcpLossMonitor`
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports attached state`
  - Verify: `build`, `test`
- [ ] 7 Add regression coverage proving the tcp retransmit monitor has a real server consumer
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF monitor performance metrics` | `Successful map read updates metrics`
  - Verify: `test`

## 4. Production health query

- [ ] 8 Add and register `GetEbpfMonitorHealth`, serialize six snapshots deterministically and return D-Bus errors for invalid context
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF health query` | `Invalid service context`
  - Verify: `build`, `test`
- [ ] 9 Add a representative client command that invokes the production D-Bus method and validates six entries
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF health query` | `Query all monitor health`
  - Verify: `build`, `behavior`
- [ ] 10 Add D-Bus and client regression tests for complete and partial availability
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF health query` | `Partial eBPF availability`
  - Verify: `test`

## 5. Verification and documentation

- [ ] 11 Run x86 build and existing unit regression, then verify the representative health-query consumer
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF health query` | `Query all monitor health`
  - Verify: `build`, `test`, `behavior`
- [ ] 12 Update README, architecture documentation and Skills/OpenSpec guide with the six-monitor contract and query command
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports stopped state`
  - Verify: `build`
- [ ] 13 Run ARM64 eBPF build and independent evaluation of all six monitors and the D-Bus surface
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `Unified eBPF monitor observability` | `Monitor reports fallback state`
  - Covers: `specs/ebpf-observability/spec.md` | `ADDED` | `eBPF monitor performance metrics` | `Successful map read updates metrics`
  - Verify: `build`, `behavior`
