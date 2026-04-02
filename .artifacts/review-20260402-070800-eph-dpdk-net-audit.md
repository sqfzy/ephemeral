# Code Audit: eph-dpdk & eph-net
**Date**: 2026-04-02 07:08
**Scope**: Full-project audit of eph-dpdk and eph-net libraries

## Summary
| Library | Critical | Major | Minor |
|---------|----------|-------|-------|
| eph-dpdk | 1 | 7 | 6 |
| eph-net | 3 | 6 | 6 |

## eph-dpdk Findings

### [C1] clamp_desc overflow past nb_max after alignment rounding
- **File**: platform.hpp:73-78
- **Fix**: Re-clamp after alignment: `n = std::min(n, nb_max);`

### [M1] rcv_wnd_ is uint16_t but recv_window config is uint32_t
- **File**: tcp.hpp:292,957
- **Fix**: Add explicit static_cast or change config type

### [M2] configure_port uses unclamped queue counts for setup_queues
- **File**: platform.hpp:335-344,392-415
- **Fix**: Store clamped values back into config

### [M3] DNS parser lacks bounds check for variable IP headers (IHL)
- **File**: dns.hpp:432-468
- **Fix**: Add bounds check after computing ihl

### [M4] Reactor mark_reconnected has data race on tuple
- **File**: reactor.hpp:199-215
- **Fix**: Document/enforce quiet period or add fence

### [M5] build_data_packet updates snd_nxt_ before TX
- **File**: tcp.hpp:588-601
- **Fix**: Document that caller MUST transmit

### [M6] Missing FIN_WAIT_1 -> FIN_WAIT_2 transition
- **File**: tcp.hpp:654-668
- **Fix**: Add state transition on ACK of FIN

### [M7] LastAck never transitions to Closed
- **File**: tcp.hpp:920-924
- **Fix**: Add LastAck -> Closed transition on ACK

### [M8] MulticastReceiver stats data race
- **File**: multicast.hpp:551,699
- **Fix**: Use std::atomic<uint64_t> for stats

### [m1] next_valid_pool_size is dead code (platform.hpp:57-62)
### [m2] Window Scale advertised but peer scale not applied (net_header.hpp:189)
### [m3] parse_ipv4 returns 0 for "0.0.0.0" — document behavior
### [m4] dns::encode_qname no output bounds check
### [m5] configure_rss timing not enforced
### [m6] Duplicate UdpHeader in dns.hpp and multicast.hpp

## eph-net Findings

### [C1] Integer overflow (UB) in proxy port parser
- **File**: proxy.hpp:604
- **Fix**: Use std::from_chars instead of manual loop

### [C2] HTTP CONNECT discards tunneled data after response headers
- **File**: proxy.hpp:406-418
- **Fix**: Use HandshakeIO for reads or read byte-by-byte

### [C3] poll_rx spins without backoff in proxy handshake
- **File**: proxy.hpp:120-143, proxy.hpp:406
- **Fix**: Use poll_rx_for() or insert ::poll() before retry

### [M1] Gateway calls stop() while holding mutex — deadlock risk
- **File**: gateway.hpp:200-209
- **Fix**: Snapshot under lock, call stop outside

### [M2] KillSwitch shutdown() partial failure leaves transports alive
- **File**: kill_switch.hpp:133-156
- **Fix**: Continue to next transport on failure

### [M3] HttpClient::tcp_connect() blocking getaddrinfo()
- **File**: http_client.hpp:419
- **Fix**: Use async getaddrinfo with timeout

### [M4] ssl_recv_all hangs with negative poll timeout
- **File**: http_client.hpp:732-737
- **Fix**: Check deadline before poll call

### [M5] recv_all treats timeout as success (truncated response)
- **File**: http_client.hpp:694-698
- **Fix**: Check is_response_complete after timeout break

### [M6] Gateway::start_all sets Healthy unconditionally
- **File**: gateway.hpp:185-196
- **Fix**: Check is_running_fn after start

### [m1] parse_proxy_url empty port gives misleading error
### [m2] SslDeleter calls SSL_shutdown without state check
### [m3] CircuitBreaker failure_count not reset on trip
### [m4] send returns error without reporting bytes sent
### [m5] http_connect_handshake doesn't use HandshakeIO for reads
### [m6] Gateway::dump() calls is_running_fn under mutex
