# Second Pass Code Audit: eph-dpdk & eph-net

**Date**: 2026-04-02
**Scope**: Error recovery, protocol edge cases, thread safety, integer arithmetic, resource ownership, cross-module contracts
**Baseline**: All findings from first-pass audit (4 critical, 13 major) already fixed

---

## Findings

### [M1] SocketTransport::close() immediately closes fd, clobbering TCP close state machine
- **Severity**: Major
- **File**: `eph-net/include/eph/net/socket_transport.hpp:550-573`
- **What's wrong**: `close()` sets `state_` to `FinWait1` or `LastAck` (lines 564-568), then immediately calls `close_fd()` (line 571), which does `::close(fd_)` and sets `state_ = TcpState::Closed` (line 712). The FIN_WAIT_1/LastAck state is instantly overwritten. After `close()` returns, `state_` is always `Closed` and `fd_` is -1, so any subsequent `poll_rx()` call (which Transport's RX thread may still be doing) will fail immediately. More importantly, the `::shutdown(SHUT_WR)` sends a FIN but the immediate `::close(fd_)` means the peer's ACK of our FIN and the peer's FIN are never received -- the kernel handles it, but the application-level state machine is broken. This means `poll_rx()` cannot drain the peer's final data after `close()`.
- **Fix**: Don't call `close_fd()` inside `close()`. Leave the fd open so `poll_rx()` can still receive the peer's FIN and transition through the state machine. Call `close_fd()` only when the full close sequence completes (state reaches Closed) or from the destructor/reset.

### [M2] TcpSession::process_rx does not handle CloseWait state -- data after FIN is silently dropped
- **Severity**: Major
- **File**: `eph-dpdk/include/eph/dpdk/tcp.hpp:682-685`
- **What's wrong**: The sequence number ordering / data delivery block at line 682 only runs in states `Established`, `FinWait1`, `FinWait2`, `Closing`. The `CloseWait` state is omitted. In CloseWait, the peer has sent a FIN, but our side hasn't sent one yet. If the peer retransmits data segments that arrived before the FIN (or if there are reordered segments still in flight), they would bypass this block entirely -- no ACK would be sent, and the data would not be processed. While CloseWait is typically brief, the omission means reordered data preceding the FIN could be lost.
- **Fix**: Add `state_ == TcpState::CloseWait` to the condition at line 682.

### [M3] DNS resolver matching packet by tx_id before verifying UDP length against mbuf data_len
- **Severity**: Major
- **File**: `eph-dpdk/include/eph/dpdk/dns.hpp:460-464`
- **What's wrong**: In `try_parse_dns_packet()`, after extracting the UDP header pointer (line 455), the code reads `udp->length` (line 462) and computes `dns_len = udp_len - kUdpHeaderLen` (line 464). But there is no check that `net::kEtherHeaderLen + ihl + udp_len <= mbuf->data_len`. A malicious or truncated packet could have a UDP length field claiming more data than the mbuf actually contains. The `parse_dns_response()` function then reads up to `dns_len` bytes from the DNS payload pointer, potentially reading past the end of the mbuf data buffer.
- **Fix**: Add a bounds check after line 463:
  ```cpp
  if (net::kEtherHeaderLen + ihl + udp_len > mbuf->data_len) return std::nullopt;
  ```

### [M4] Reactor entry tuple and hashes_ written non-atomically during mark_reconnected -- stale hash causes missed packet delivery for one burst cycle
- **Severity**: Major (data loss window)
- **File**: `eph-dpdk/include/eph/dpdk/reactor.hpp:209-210`
- **What's wrong**: In `mark_reconnected()`, step 3 updates `entries_[conn_id].tuple` and `hashes_[conn_id]` non-atomically. The comment on line 197 says "a stale value causes at most one missed or extra packet, not UB". This is accurate -- there is no UB. However, the tuple update is a multi-word write (`ConnectionTuple` is 12 bytes: 2x uint32_t + 2x uint16_t). The RX thread reads `entries_[j].tuple` at line 287 (`parsed.matches(entries_[j].tuple)`). If the RX thread reads a half-written tuple (e.g., new src_ip but old dst_ip), the match could produce a false positive on the wrong connection, delivering data to the wrong session's `on_data` callback. This is worse than "one missed packet" -- it's data corruption.
- **Fix**: Use a local `ConnectionTuple` copy and update the entry atomically via a seqlock, or require callers to stop the reactor before calling `mark_reconnected` (document this restriction). Alternatively, since tuple is only 12 bytes, store it in an `alignas(16) std::atomic<ConnectionTuple>` using lock-free 128-bit CAS on x86_64.

### [M5] TcpSession move constructor/assignment does not clear source reorder_buf_ entries
- **Severity**: Major
- **File**: `eph-dpdk/include/eph/dpdk/tcp.hpp:332-356, 358-382`
- **What's wrong**: The move constructor copies `reorder_count_` entries from `other.reorder_buf_[]` and sets `other.reorder_count_ = 0`. However, it does not clear the copied entries in `other.reorder_buf_[]`. This leaves stale `ReorderEntry` data in the moved-from session. If the moved-from session is somehow reused (e.g., passed back to a pool), or if the destructor interacts with the reorder buffer, stale entries with valid `seq`/`len` fields could cause incorrect delivery. While `reorder_count_ = 0` prevents immediate access, the invariant that "entries beyond reorder_count_ are zeroed" is violated.
- **Fix**: Zero `other.reorder_buf_[i]` entries after copying, or document that moved-from TcpSession must not be reused without full re-initialization via `connect()`.

### [M6] Connector DNS-fallback path creates Platform then calls connect() overload that creates a second Platform
- **Severity**: Major (resource leak / NIC conflict)
- **File**: `eph-dpdk/include/eph/dpdk/connector.hpp:540`
- **What's wrong**: In the DNS-fallback `connect()` overload (line 469), the code creates a Platform at line 501, uses it for ARP and DNS resolution, then calls `connect<TransportType>(*platform, ep, transport_cfg, *dpdk_ip, opts_with_mac)` at line 540. This correctly calls the `connect(Platform&, ...)` overload. However, the returned `ConnectResult` at line 548 `std::move(*platform)` moves the Platform into the result. The problem: the local `platform` variable was created via `Platform::create()` at line 501, and the `prepare_connection()` call inside the `connect(Platform&, ...)` overload at line 591 calls `rte_eth_macaddr_get(opts.platform.port_id, &src_mac)` using `opts.platform.port_id` -- NOT `platform.port_id()`. If the user passed `ConnectorOptions` with a **different** `platform.port_id` than what was created, the MAC lookup targets the wrong port. This is a contract violation between the Platform and ConnectorOptions.
- **Fix**: In `prepare_connection()`, use the passed `platform.port_id()` (via a parameter) instead of `opts.platform.port_id` for the MAC lookup. Or validate that they match.

### [m1] PacketTemplate::ip_id wraps at uint16_t max with no reset on reconnect
- **Severity**: Minor
- **File**: `eph-dpdk/include/eph/dpdk/net_header.hpp:227, 281, 374`
- **What's wrong**: `ip_id` is a `uint16_t` that increments on every packet sent (`ip_id++` at lines 281 and 374). When it wraps from 65535 to 0, the IP identification field restarts. This is technically correct per RFC (IP ID is allowed to wrap), but after a reconnect, `ip_id` is NOT reset -- it continues from where it left off. If the old and new connections share the same 4-tuple (common with ephemeral port reuse), stale fragments from the old connection could match new IP IDs. With DF (Don't Fragment) set (line 375), fragmentation shouldn't occur, so this is a theoretical concern only.
- **Fix**: Reset `ip_id = 0` in `TcpSession::connect()` alongside ISN regeneration, or document the DF reliance.

### [m2] dns::resolve busy-loops without yield/pause between poll iterations
- **Severity**: Minor
- **File**: `eph-dpdk/include/eph/dpdk/dns.hpp:572-631`
- **What's wrong**: The resolve loop at line 572 busy-polls `rte_eth_rx_burst` without any pause/yield between iterations when `now < next_send` and `nb_rx == 0`. On systems without the NIC (e.g., testing), this consumes 100% CPU for the full timeout duration. The same issue exists in `arp::resolve()` at line 243. Both are pre-connection setup paths (not hot path), so CPU waste during blocking resolution is the concern.
- **Fix**: Add `rte_delay_us(1)` or `_mm_pause()` when `nb_rx == 0` to reduce CPU waste during polling.

### [m3] TcpSession constructor silently accepts null mempool and proceeds
- **Severity**: Minor
- **File**: `eph-dpdk/include/eph/dpdk/tcp.hpp:295-299`
- **What's wrong**: The constructor checks `if (!pool_)` and logs an error, but does not fail or throw. The session is created in a broken state where every subsequent packet allocation will fail. The `TcpConfig::validate()` method does not validate the pool either (it's not part of TcpConfig). This means `connect()` will only fail later with "mbuf allocation failed for SYN" -- a less actionable error message.
- **Fix**: Either assert/abort on null pool in the constructor, or add a factory function that returns `std::expected` and rejects null pool upfront.

### [m4] SocketTransport::poll_rx truncates recv to uint16_t without check
- **Severity**: Minor
- **File**: `eph-net/include/eph/net/socket_transport.hpp:491-493`
- **What's wrong**: When `recv()` returns `n > 0`, the callback is invoked with `static_cast<uint16_t>(n)`. The recv buffer is 16384 bytes, which fits in uint16_t (max 65535). However, the function returns `static_cast<uint16_t>(1)` (line 493) as a count of "data packets processed" -- always 1 regardless of how many bytes were received. This is correct for the TcpTransport concept (which expects a packet count, not byte count), but if recv returns data larger than 65535 bytes (impossible with the 16384 buffer, but fragile if buffer size changes), the uint16_t cast would silently truncate the length.
- **Fix**: Add `static_assert(sizeof(buf) <= UINT16_MAX)` to guard against future buffer size changes.

### [m5] KillSwitch destructor calls shutdown() which calls spdlog -- unsafe in static destruction order
- **Severity**: Minor
- **File**: `eph-net/include/eph/net/kill_switch.hpp:75-81`
- **What's wrong**: If a `KillSwitch` is a global/static object, its destructor runs during static destruction. The `shutdown()` call invokes `SPDLOG_LOGGER_INFO` which accesses the static `kill_switch_logger()` singleton. If spdlog's global registry has already been destroyed (static destruction order is unspecified between TUs), this is use-after-free. The signal handler deregistration at lines 79-80 is safe (signal/atomic are async-signal-safe), but the `shutdown()` call is not.
- **Fix**: Guard spdlog calls in `shutdown()` with a check for spdlog registry validity, or document that KillSwitch must not be a static/global object.

---

## Summary

| Severity | Count | Key Themes |
|----------|-------|------------|
| Major    | 6     | State machine gaps (M1, M2), protocol bounds (M3), concurrency (M4), move semantics (M5), cross-module contract (M6) |
| Minor    | 5     | Robustness guards (m1-m4), static destruction (m5) |

The most impactful findings are **M1** (SocketTransport close breaks poll_rx drain) and **M3** (DNS mbuf over-read). M4 (reactor tuple tear) is a real data corruption risk but only during live reconnection under load.
