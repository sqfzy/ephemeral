# Troubleshooting Guide

Error diagnosis reference for ephemeral. Maps error codes to causes and fixes.

## Connection Errors (`ConnectionError`)

Returned by `Transport::create()` and reconnection attempts.

### INVALID_CONFIG

**Cause**: `TransportConfig` validation failed before any network I/O.

| Detail message | Fix |
|----------------|-----|
| `remote_host is empty` | Set `cfg.remote_host` |
| `pong_timeout must be >= ping_interval` | Increase `pong_timeout` or decrease `ping_interval` |
| `client_cert_path set without client_key_path` | Set both or neither for mTLS |

### FACTORY_FAILED

**Cause**: The `TcpFactory` lambda returned an error. For socket backend, this usually means TCP connect failed.

| Detail pattern | Likely cause | Fix |
|----------------|-------------|-----|
| `Connection refused` | Server not listening on that port | Verify host:port, check firewall |
| `Connection timed out` | Network unreachable or blocked | Check routing, VPC security groups |
| `Name resolution failed` | DNS lookup failed | Verify hostname, check DNS config |
| `Network is unreachable` | No route to host | Check network interface, gateway |

### TCP_NOT_ESTABLISHED

**Cause**: TcpFactory returned a socket that isn't in ESTABLISHED state.

**Fix**: Ensure your TcpFactory calls `tcp->connect()` and checks the result before returning.

### TLS_SESSION_FAILED

**Cause**: TLS session object creation failed (before handshake).

**Common causes**:
- aws-lc library not linked or incompatible version
- Invalid CA cert path (`ca_cert_path` points to nonexistent file)
- Invalid client cert/key for mTLS

### TLS_HANDSHAKE_FAILED

**Cause**: TLS handshake failed after TCP connection established.

| Detail pattern | Likely cause | Fix |
|----------------|-------------|-----|
| `certificate verify failed` | Server cert not trusted | Set `ca_cert_path` to correct CA bundle, or check cert expiry |
| `handshake timeout` | Server slow to respond | Increase `tls_timeout` |
| `protocol version` | TLS version mismatch | ephemeral requires TLS 1.3; ensure server supports it |
| `self-signed certificate` | Dev/staging server | Set `verify_peer = false` for testing only |

**Diagnosis steps**:
1. Test with `openssl s_client -connect host:port -tls1_3`
2. Check cert expiry: `openssl s_client -connect host:port 2>/dev/null | openssl x509 -noout -dates`
3. Check system clock: TLS certs are time-sensitive

### TLS_KEY_EXPORT_FAILED

**Cause**: Could not export AEAD keys after successful handshake. Internal error.

**Fix**: Report as bug with the TLS library version (`openssl version`).

### WS_UPGRADE_FAILED

**Cause**: HTTP upgrade request sent but response parsing failed.

| Detail pattern | Likely cause | Fix |
|----------------|-------------|-----|
| `timeout waiting for upgrade response` | Server didn't respond | Increase `ws_timeout`, verify `ws_path` |
| `incomplete HTTP response` | Connection closed mid-handshake | Check if server supports WebSocket |
| `missing Upgrade/Connection headers` | Server responded but not with WS upgrade | Verify endpoint is a WebSocket URL |

### WS_UPGRADE_REJECTED

**Cause**: Server responded with a non-101 status code.

| HTTP Status | Meaning | Fix |
|-------------|---------|-----|
| 400 | Bad request | Check `ws_path`, `extra_headers` |
| 401 / 403 | Auth required/forbidden | Add auth token to `extra_headers` |
| 404 | Wrong path | Fix `ws_path` |
| 429 | Rate limited | Add backoff, reduce connection frequency |
| 503 | Server overloaded | Retry later |

**Access the status code programmatically**:
```cpp
auto result = Transport<...>::create(factory, cfg);
if (!result && result.error().code == ConnectionError::kWsUpgradeRejected) {
    if (result.error().http_status == 429) {
        // Rate limited — back off
    }
}
```

### WS_ACCEPT_INVALID

**Cause**: Server's `Sec-WebSocket-Accept` header doesn't match expected SHA-1 hash.

**Likely cause**: Proxy or CDN modifying WebSocket headers.

**Fix**: Connect directly (bypass proxy) or configure proxy for WebSocket passthrough.

---

## Send Errors (`SendError`)

Returned by `send()`, `send_text()`, `send_binary()`.

| Error | Cause | Fix |
|-------|-------|-----|
| `MESSAGE_TOO_LARGE` | Payload exceeds `MaxPayload` template parameter | Increase MaxPayload or split message |
| `NOT_CONNECTED` | Transport not running or disconnected | Check `is_connected()` before sending, or handle reconnect |
| `QUEUE_FULL` | TX queue is full (backpressure) | Use `send_for()` with timeout, or check `tx_queue_occupancy()` |
| `INVALID_UTF8` | Text frame payload fails UTF-8 validation | Fix payload encoding, or set `skip_utf8_validation = true` |
| `INVALID_CLOSE_CODE` | Close code not in RFC 6455 valid range | Use `ws::close_code::kNormal` (1000) or other valid codes |
| `NULL_DATA` | `data` is nullptr but `len > 0` | Fix caller to provide valid data pointer |

---

## Runtime Issues

### Connection drops every N minutes

**Symptom**: Transport reconnects periodically with `on_state_change(kDisconnected)`.

**Diagnosis**:
1. Check if server sends Close frames: register `on_close` callback
2. Check ping/pong health: register `on_pong` callback, measure RTT
3. Check TLS sequence exhaustion: look for `TLS write sequence at 90%` log warning
4. Check server-side idle timeout: many servers close connections after 5-30 min of no activity

**Fixes**:
- Reduce `ping_interval` to keep connection alive
- If TLS sequence warning appears: reconnect proactively before reaching limit

### RX queue drops (messages lost)

**Symptom**: `on_rx_drop` callback fires, messages missing.

**Diagnosis**: Application thread not consuming fast enough.

**Fixes**:
- Use `on_message` push callback instead of `recv()` polling (eliminates queue entirely)
- Increase queue depth (template parameter `QueueDepth`)
- Use `EvictingQueue` (template parameter `RxQueueTmpl`) to keep latest and discard old
- Profile the application's message processing — is it doing I/O in the hot loop?

### High tail latency (p99 spikes)

**Diagnosis**:
1. Check CPU pinning: `tx_cpu`/`rx_cpu` must be on isolated cores (not shared with OS)
2. Check NUMA locality: TX/RX cores should be on same NUMA node as NIC
3. Check for thermal throttling: `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`
4. Check for context switches: `perf stat -e context-switches ./your_app`

**Fixes**:
- Isolate cores: `isolcpus=2,3` in kernel boot params
- Disable turbo boost for consistent frequency: `echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`
- Use `perf_tuning_basics` example to validate CPU topology before deployment

### DPDK: connection fails immediately

**Diagnosis**:
- Check EAL init: `rte_eal_init()` must succeed before any DPDK operations
- Check NIC binding: `dpdk-devbind.py --status` — NIC must be bound to DPDK-compatible driver
- Check hugepages: `cat /proc/meminfo | grep HugePages` — need at least 1024 2MB pages
- Check ARP: gateway MAC must be resolved before TCP connect

**Fixes**:
```bash
# Allocate hugepages
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Bind NIC to vfio-pci
modprobe vfio-pci
dpdk-devbind.py -b vfio-pci 0000:00:05.0

# Verify
dpdk-devbind.py --status
```

---

## Log Levels

Set spdlog level to control verbosity:

```cpp
spdlog::set_level(spdlog::level::debug);  // Full debug output
spdlog::set_level(spdlog::level::info);   // Normal operation
spdlog::set_level(spdlog::level::warn);   // Only warnings and errors
```

Key log channels:
- `net.transport` — connection lifecycle, reconnection, state changes
- `net.websocket` — WS frame encoding/decoding issues
- `net.tls` — TLS handshake, encryption errors, sequence warnings
- `net.http` — HTTP upgrade request/response
- `fix.parser` / `fix.builder` — FIX message validation errors
- `itch.parser` — ITCH parse errors (unknown type, truncated)
