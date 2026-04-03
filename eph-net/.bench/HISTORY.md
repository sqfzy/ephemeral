# Benchmark History -- eph-net

## 2026-04-03 bench_http_client (initial baselines)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_BuildHttpRequest_Get | 57.3 | 57.3 |
| BM_BuildHttpRequest_Post | 160 | 160 |
| BM_ParseHttpResponse_Json | 81.3 | 81.3 |
| BM_ParseHttpResponse_LargeBody | 86.5 | 86.5 |
| BM_FindHeader | 77.3 | 77.3 |
| BM_FindHeader_LastHeader | 67.2 | 67.2 |
| BM_FindHeader_Miss | 22.7 | 22.7 |
| BM_HttpResponse_ToJson | 292 | 292 |
| BM_HttpClientConfig_Validate | 7.15 | 7.15 |

## 2026-04-03 bench_proxy (initial baselines)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_ParseProxyUrl_Socks5NoAuth | 46.0 | 46.0 |
| BM_ParseProxyUrl_Socks5WithAuth | 62.7 | 62.7 |
| BM_ParseProxyUrl_HttpConnect | 31.3 | 31.3 |
| BM_ProxyConfig_Validate | 8.58 | 8.58 |
| BM_ProxyConfig_ToUrl | 145 | 145 |
| BM_ProxyConfig_ToJson | 210 | 210 |
| BM_ProxyConfig_Dump | 226 | 226 |

## 2026-04-03 bench_socket_config (initial baselines)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_SocketConfig_FromUrl_Simple | 30.1 | 30.1 |
| BM_SocketConfig_FromUrl_TcpScheme | 42.0 | 42.0 |
| BM_SocketConfig_FromUrl_IPv6 | 28.1 | 28.1 |
| BM_SocketConfig_ToUrl | 82.6 | 82.6 |
| BM_SocketConfig_Validate | 7.86 | 7.86 |
| BM_SocketConfig_ToJson | 403 | 403 |
| BM_SocketConfig_Dump | 356 | 356 |

## 2026-04-03 bench_kill_switch (after KillSwitch::to_json)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_KillSwitch_IsShutdownRequested | 0.363 | 0.363 |
| BM_KillSwitch_Register | 3139 | 3165 |
| BM_KillSwitch_Unregister | 224 | 225 |
| BM_KillSwitch_TransportCount | 0.358 | 0.358 |
| BM_KillSwitch_RequestShutdown | 0.358 | 0.358 |
| BM_KillSwitch_ToJson | 139 | 139 |

## 2026-04-03 bench_gateway (after Gateway::to_json)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_Gateway_ToJson/1 | 475 | 475 |
| BM_Gateway_ToJson/4 | 893 | 893 |
| BM_Gateway_ToJson/8 | 1442 | 1442 |

## 2026-04-03 bench_control_plane (after RateLimiter to_json/config parity)

| Benchmark | Time (ns) | CPU (ns) |
|---|---|---|
| BM_RateLimiter_TryAcquire_Available | 42.5 | 42.5 |
| BM_RateLimiter_TryAcquire_Exhausted | 42.8 | 42.8 |
| BM_RateLimiter_Available | 39.4 | 39.4 |
| BM_CircuitBreaker_Allow_Closed | 5.36 | 5.36 |
| BM_CircuitBreaker_Allow_Open | 37.4 | 37.4 |
| BM_CircuitBreaker_RecordSuccess_Closed | 5.38 | 5.38 |
| BM_CircuitBreaker_RecordFailure_Closed | 5.37 | 5.37 |
| BM_CircuitBreaker_State | 5.06 | 5.06 |
| BM_CircuitBreaker_Reset | 1642 | 1642 |
| BM_RateLimiter_Reset | 36.9 | 36.9 |
| BM_CircuitBreakerConfig_Validate | 0.357 | 0.357 |
| BM_RateLimiterConfig_Validate | 0.357 | 0.357 |
| BM_RateLimiter_ToJson | 345 | 345 |
| BM_CircuitBreaker_ToJson | 294 | 294 |
