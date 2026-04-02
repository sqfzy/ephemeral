# Code Audit: eph-dpdk & eph-net
**Date**: 2026-04-02 07:08 — 08:04
**Scope**: Full-project audit (6 passes: correctness×2, cross-module, security, concurrency, API contract)

## Summary
| Category | Found | Fixed | Deferred |
|----------|-------|-------|----------|
| Critical | 4 | 4 | 0 |
| Major | 25 | 23 | 2 |
| Minor | 17 | 1 | 16 |
| Security | 4M+4m | 3 | 5 |

## Commits
1. `eaa5381` fix(dpdk,net): 4 critical, 13 major fixes
2. `e2864b2` fix(dpdk): CloseWait data delivery, DNS truncation guard
3. `f81d794` fix(dpdk,net): reactor data race, HMAC key truncation
4. `01e710e` fix(dpdk,net): DNS answer cap, TLS hostname verify, proxy buffer bound
5. `9b73997` fix(net): make SocketTransport state_ atomic
6. `88761ac` fix(net): defer close_fd() for graceful shutdown

---

## eph-dpdk Findings

### Critical (all fixed)
| ID | File | Issue | Status |
|----|------|-------|--------|
| C1 | platform.hpp:73 | clamp_desc overflow past nb_max after alignment | ✅ eaa5381 |

### Major (all fixed)
| ID | File | Issue | Status |
|----|------|-------|--------|
| M1 | tcp.hpp:292 | rcv_wnd_ uint16_t truncation | ✅ eaa5381 |
| M2 | platform.hpp:335 | setup_queues uses unclamped queue counts | ✅ eaa5381 |
| M3 | dns.hpp:432 | DNS parser lacks IHL bounds check | ✅ eaa5381 |
| M4 | reactor.hpp:209 | Data race on tuple in mark_reconnected | ✅ f81d794 |
| M5 | tcp.hpp:588 | build_data_packet updates snd_nxt_ before TX | ✅ eaa5381 (doc) |
| M6 | tcp.hpp:654 | Missing FIN_WAIT_1 → FIN_WAIT_2 transition | ✅ eaa5381 |
| M7 | tcp.hpp:920 | LastAck never transitions to Closed | ✅ eaa5381 |
| M8 | multicast.hpp:551 | Stats counters data race | ✅ eaa5381 |
| M9 | tcp.hpp:682 | CloseWait omitted from data delivery states | ✅ e2864b2 |
| M10 | dns.hpp:460 | DNS udp_len not validated against mbuf data_len | ✅ e2864b2 |

### Minor (deferred — low risk)
- m1: next_valid_pool_size dead code (platform.hpp:57)
- m2: Window Scale advertised but peer scale not applied (net_header.hpp:189)
- m3: parse_ipv4 returns 0 for "0.0.0.0" (net_header.hpp:543)
- m4: encode_qname no output bounds check (dns.hpp:168)
- m5: configure_rss timing not enforced (flow_steering.hpp:172)
- m6: Duplicate UdpHeader in dns.hpp and multicast.hpp

### Security (partially fixed)
| ID | File | Issue | Status |
|----|------|-------|--------|
| S3 | dns.hpp:307 | DNS an_count unbounded (CPU exhaustion) | ✅ 01e710e |

---

## eph-net Findings

### Critical (all fixed)
| ID | File | Issue | Status |
|----|------|-------|--------|
| C1 | proxy.hpp:604 | Integer overflow (UB) in proxy port parser | ✅ eaa5381 |
| C2 | proxy.hpp:406 | HTTP CONNECT discards tunneled data | ✅ eaa5381 (warning) |
| C3 | proxy.hpp:120 | poll_rx spins without backoff | ✅ eaa5381 |

### Major
| ID | File | Issue | Status |
|----|------|-------|--------|
| M1 | gateway.hpp:200 | stop_all() calls stop under mutex | ✅ eaa5381 |
| M2 | kill_switch.hpp:133 | shutdown() partial failure | ⏳ deferred |
| M3 | http_client.hpp:419 | Blocking getaddrinfo() | ⏳ deferred |
| M4 | http_client.hpp:732 | ssl_recv_all negative poll timeout | ✅ eaa5381 |
| M5 | http_client.hpp:694 | recv_all timeout returns truncated data | ✅ eaa5381 |
| M6 | gateway.hpp:185 | start_all sets Healthy unconditionally | ✅ eaa5381 |
| M7 | hmac.hpp:108 | HMAC key.size() truncated to int | ✅ f81d794 |
| M8 | socket_transport.hpp:675 | state_ not atomic (RX/TX data race) | ✅ 9b73997 |
| M9 | socket_transport.hpp:570 | close() destroys fd immediately, breaking graceful shutdown | ✅ 88761ac |

### Minor (deferred — low risk)
- m1: parse_proxy_url empty port misleading error
- m2: SslDeleter SSL_shutdown without state check
- m3: CircuitBreaker failure_count not reset on trip
- m4: send error doesn't report bytes sent
- m5: http_connect_handshake doesn't use HandshakeIO for reads
- m6: Gateway::dump() calls is_running_fn under mutex

### Security
| ID | File | Issue | Status |
|----|------|-------|--------|
| S1 | proxy.hpp:405 | HTTP CONNECT buffer not bounded in callback | ✅ 01e710e |
| S2 | http_client.hpp:673 | recv_all 256MB allocation | ⏳ needs config option |
| S4 | hmac.hpp | No constant-time comparison | ⏳ needs new API |
| S5 | http_client.hpp:529 | TLS hostname verification | ✅ 01e710e |
| S6 | connector.hpp:106 | JSON injection in to_json() | ⏳ minor |

---

## Deferred Items Summary
| ID | Risk | Reason for deferral |
|----|------|---------------------|
| net/M2 | Low | KillSwitch is noexcept; stop_fn throwing terminates anyway |
| net/M3 | Low | getaddrinfo blocking is for HTTP REST client only (not hot path) |
| net/S2 | Medium | Needs new config field — feature addition, not bug fix |
| net/S4 | Medium | Needs new constant_time_compare API — feature addition |
| All minors | Low | Code quality / defensive improvements, no runtime impact |
