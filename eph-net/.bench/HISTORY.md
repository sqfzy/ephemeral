# Benchmark History — eph-net

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
