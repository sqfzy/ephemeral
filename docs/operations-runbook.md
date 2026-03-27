# Operations Runbook

Monitoring, alerting, and incident response for ephemeral in production.

## Metrics Export

### TransportStats Snapshot

```cpp
// Periodic stats export (every 1 second)
auto prev = transport->stats();
while (running) {
    std::this_thread::sleep_for(1s);
    auto curr = transport->stats();
    auto delta = curr - prev;
    prev = curr;

    // Export to your monitoring system
    metrics.gauge("eph.tx_packets",    delta.tx.packets);
    metrics.gauge("eph.rx_packets",    delta.rx.packets);
    metrics.gauge("eph.tx_bytes",      delta.tx.bytes);
    metrics.gauge("eph.rx_bytes",      delta.rx.bytes);
    metrics.gauge("eph.tx_pps",        curr.tx_pps());
    metrics.gauge("eph.rx_pps",        curr.rx_pps());
    metrics.gauge("eph.tx_bps",        curr.tx_bps());
    metrics.gauge("eph.rx_bps",        curr.rx_bps());
    metrics.gauge("eph.uptime_s",      curr.uptime_ns() / 1e9);
    metrics.gauge("eph.reconnects",    curr.reconnect_count);
    metrics.gauge("eph.rx_drops",      delta.rx.drops);
    metrics.gauge("eph.crypto_errors", delta.rx.crypto_errors);
}
```

### ConnectionInfo

```cpp
auto info = transport->connection_info();
// info.tls_version    — "TLSv1.3"
// info.tls_cipher     — "TLS_AES_256_GCM_SHA384"
// info.remote_addr    — "1.2.3.4:443"
// info.ws_subprotocol — negotiated subprotocol (if any)
```

### Queue Occupancy

```cpp
float tx_pct = transport->tx_queue_occupancy();  // 0.0 - 1.0
float rx_pct = transport->rx_queue_occupancy();  // 0.0 - 1.0
metrics.gauge("eph.tx_queue_pct", tx_pct * 100);
metrics.gauge("eph.rx_queue_pct", rx_pct * 100);
```

## Alert Rules

### Prometheus/Alertmanager Examples

```yaml
groups:
  - name: ephemeral
    rules:
      # Connection down for >30 seconds
      - alert: EphConnectionDown
        expr: eph_uptime_s == 0
        for: 30s
        labels: { severity: critical }
        annotations:
          summary: "ephemeral transport disconnected"

      # High reconnect rate
      - alert: EphReconnectStorm
        expr: rate(eph_reconnects[5m]) > 0.1
        labels: { severity: warning }
        annotations:
          summary: "{{ $value }} reconnects/sec — check network stability"

      # RX queue backpressure (>80% full)
      - alert: EphRxQueueFull
        expr: eph_rx_queue_pct > 80
        for: 10s
        labels: { severity: warning }
        annotations:
          summary: "RX queue at {{ $value }}% — consumer may be too slow"

      # RX drops detected
      - alert: EphRxDrops
        expr: rate(eph_rx_drops[1m]) > 0
        labels: { severity: warning }
        annotations:
          summary: "{{ $value }} drops/sec — consider EvictingQueue or faster consumer"

      # TX queue backpressure
      - alert: EphTxQueueFull
        expr: eph_tx_queue_pct > 90
        for: 5s
        labels: { severity: critical }
        annotations:
          summary: "TX queue at {{ $value }}% — messages may be lost on disconnect"

      # Crypto errors (TLS decrypt failures)
      - alert: EphCryptoErrors
        expr: rate(eph_crypto_errors[5m]) > 0
        labels: { severity: critical }
        annotations:
          summary: "TLS decrypt errors detected — possible key exhaustion or corruption"
```

## Incident Playbooks

### Playbook: Connection Flapping

**Symptom**: `on_state_change` alternates between `kConnected` and `kDisconnected` every few seconds.

**Triage**:
1. Check `connection_info()` — is TLS version/cipher as expected?
2. Check reconnect callback — what error triggered reconnection?
3. Check server-side logs — is the server actively closing connections?
4. Check network path — `mtr host` for packet loss

**Resolution**:
- If server is closing due to idle timeout → decrease `ping_interval`
- If server is rate-limiting connections → increase `reconnect_interval`
- If TLS handshake fails intermittently → check cert expiry, clock sync
- If TCP RST from server → contact exchange support

### Playbook: Increasing Latency

**Symptom**: p99 latency drifting upward over hours/days.

**Triage**:
1. Check CPU frequency: `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`
2. Check for thermal throttling: `dmesg | grep -i throttl`
3. Check context switches: `perf stat -e context-switches -p <pid>`
4. Check NUMA locality: is the process still on the correct NUMA node?
5. Check TLS sequence numbers: look for "TLS write sequence at 90%" warning

**Resolution**:
- CPU throttled → disable turbo boost, ensure adequate cooling
- Context switch spike → verify `isolcpus`, check for competing processes
- NUMA migration → pin with `numactl --cpunodebind`
- TLS sequence warning → reconnect proactively to reset counters

### Playbook: Message Loss

**Symptom**: Application missing expected messages, `on_rx_drop` firing.

**Triage**:
1. Check `rx_queue_occupancy()` — consistently >90%?
2. Check application processing time per message
3. Check if using `BoundedQueue` (blocks) vs `EvictingQueue` (drops old)

**Resolution**:
- Consumer too slow → profile processing loop, move I/O off hot path
- Queue too small → increase `QueueDepth` template parameter
- Stale data acceptable → switch to `EvictingQueue` with `LastOnlyDeliver`
- Need all messages → increase consumer throughput (batch processing, SIMD parsing)

## Health Check Endpoint

Minimal health check for load balancers or monitoring:

```cpp
// Returns true if connected and sending/receiving within last 5 seconds
bool is_healthy(const auto& transport) {
    if (!transport.is_connected()) return false;
    auto stats = transport.stats();
    auto idle_ns = eph::utils::TSC::now() - stats.last_rx_tsc;
    return eph::utils::TSC::to_ns(idle_ns).value_or(0) < 5'000'000'000ULL;
}
```

## Graceful Shutdown Sequence

```cpp
// 1. Stop accepting new work
application.stop_accepting();

// 2. Drain TX queue
while (transport->tx_queue_occupancy() > 0) {
    std::this_thread::sleep_for(10ms);
}

// 3. Graceful WebSocket close (sends Close frame, waits for echo)
bool clean = transport->close_gracefully(
    ws::close_code::kNormal, "planned shutdown", 3000ms);

// 4. Log final stats
auto final_stats = transport->stats();
spdlog::info("Shutdown complete: TX={} RX={} reconnects={} drops={}",
    final_stats.tx.packets, final_stats.rx.packets,
    final_stats.reconnect_count, final_stats.rx.drops);
```
