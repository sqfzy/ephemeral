# mockex — unified mock server for the latency bench

`mockex` is a single C++23 binary that serves every `lat_<scenario>` bench
client. It replaces the constellation of Python scripts that used to live
under `benchmarks/latency/mocks/` so that (a) the mock's timestamps share
the bench client's toolchain (same `bench::monotonic_raw_ns()` call path),
and (b) the push-flow scenarios can emit *realistic* traffic — real Binance
`bookTicker` JSON bytes looped from a checked-in fixture, with arrival
times sampled from a two-state Markov-modulated Poisson process fitted
from a real capture.

## Layout

```
benchmarks/mockex/
  include/mockex/          header-only handlers (scenario_*.hpp, mmpp2.hpp,
                           payload_pool.hpp, push_loop.hpp, ws_server.hpp)
  src/main.cpp             CLI → dispatch table → handler
  fixtures/                checked-in *.jsonl + *.ini per push scenario
  tools/                   offline Python: capture_binance, fit_mmpp,
                           synth_fixture, ks_validate
  tests/                   gtest integration tests per scenario
  xmake.lua                single binary target + per-test targets
```

## Usage

Build and run via the `lat` wrapper — it handles NIC/namespace setup:

```bash
xmake build mockex
sudo ./benchmarks/latency/lat tcp             # plain TCP echo
sudo ./benchmarks/latency/lat ws              # plain WebSocket echo
sudo ./benchmarks/latency/lat ex_market       # MMPP-driven bookTicker push
sudo ./benchmarks/latency/lat ex_market_2p    # multi-symbol + burst
```

Or run the binary standalone for debugging:

```bash
./build/linux/arm64/release/mockex --scenario tcp \
    --config benchmarks/latency/bench.conf
```

## Real vs mock — the `endpoint` switch

Every scenario accepts an `endpoint` key in its `[lat_*]` section:

```ini
[lat_ex_market]
port           = 20003
endpoint       = mock           # default — talk to mockex on mock_ip:port
# endpoint     = wss://stream.binance.com:9443/ws/btcusdt@bookTicker
mockex_params  = benchmarks/mockex/fixtures/ex_market_params.ini
mockex_payload = benchmarks/mockex/fixtures/ex_market_sample.jsonl
```

With `endpoint = wss://…` the `lat` wrapper skips the mockex fork and the
client dials the exchange directly. This is the *validation* path:
capture a real stream, fit new mockex parameters from it, then flip back
to mock mode and use `ks_validate.py` to confirm the mockex distribution
matches real traffic.

## TLS server (optional, per-scenario)

`mockex` can wrap any TCP-based scenario in TLS by setting
`use_tls = true` in the scenario's TOML section and pointing to cert/key
paths in the global `[tls]` table:

```ini
[tls]
cert_path     = benchmarks/mockex/fixtures/tls/server.crt
key_path      = benchmarks/mockex/fixtures/tls/server.key
# Optional: minimum negotiable TLS version. Defaults to "tls13"; set
# "tls12" only when validating eph-net's TLS 1.2 GCM/CHACHA20 path
# (interop with TLS 1.2-only middleboxes).
# min_version = "tls12"

[scenarios.lat_ws]
port    = 20002
use_tls = true
```

When `min_version = "tls12"` is set, mockex installs the same AEAD-only
cipher whitelist as the eph-net client (ECDHE-{RSA,ECDSA}-AES{128,256}-GCM
+ ECDHE-{RSA,ECDSA}-CHACHA20-POLY1305). CBC suites are never accepted at
either version.

## Refit workflow

The checked-in `fixtures/*_sample.jsonl` and `*_params.ini` are
**synthetic bootstrap artefacts**. Replace them with the output of a
real-binance capture + fit before relying on any absolute latency number:

```bash
# 1. Capture ~30 min of real wire traffic (repeat across sessions for
#    representative coverage — rate shape varies with trading hours).
python3 benchmarks/mockex/tools/capture_binance.py \
    --stream btcusdt@bookTicker --duration 1800 \
    --out benchmarks/mockex/fixtures/ex_market_sample
# Writes ex_market_sample.jsonl + ex_market_sample.arrivals

# 2. Fit MMPP-2 + size KDE from the capture.
python3 benchmarks/mockex/tools/fit_mmpp.py \
    --in benchmarks/mockex/fixtures/ex_market_sample \
    --out benchmarks/mockex/fixtures/ex_market_params.ini \
    --seed 42

# 3. Sanity-check that mockex now matches the real distribution.
#    Run mockex against capture_binance to produce a mock .arrivals
#    file, then:
python3 benchmarks/mockex/tools/ks_validate.py \
    --real benchmarks/mockex/fixtures/ex_market_sample.arrivals \
    --mock /tmp/mockex_ex_market.arrivals
# K-S p > 0.05 + |ACF diff| < 0.1 → PASS
```

## The `mmpp2` model in one paragraph

Two hidden regimes (quiet / busy) with per-frame Poisson arrival rates
`λ_q` / `λ_b`; after each event the chain transitions with
`p_quiet_to_busy` / `p_busy_to_quiet`. The stationary busy fraction is
`π_b = p_qb / (p_qb + p_bq)`. For Binance bookTicker at a quiet hour
`π_b ≈ 0.02` with `λ_b ≈ 10 × λ_q`, producing visible clusters of
frames triggered by a price event followed by a chain of depth updates
— the fatter-than-Poisson tail the synthetic `push_rate_hz` mock
couldn't reproduce. Payload sizes are drawn from a KDE-style mixture
of quantile anchors fitted alongside the arrival-time parameters.

## Adding a new scenario

1. Drop `include/mockex/scenarios/<name>.hpp` implementing a
   `<name>_run(const ScenarioContext&)` handler (follow `tcp_echo.hpp`
   for echo or `ex_market_push.hpp` for push).
2. Add the entry to `kScenarioTable` in `dispatch.hpp`.
3. Add an integration test in `tests/test_mockex_<name>.cpp`.
4. Add a `[lat_<name>]` section to `benchmarks/latency/bench.conf` and a
   corresponding entry in `benchmarks/latency/lat`'s `SCENARIO_MOCKS`.
5. If the scenario is push-style, ship a synthetic
   `<name>_sample.jsonl` + `<name>_params.ini` (use `tools/fit_mmpp.py
   --synthetic <name>` to bootstrap).

## Historical note

Pre-Phase-1 the mocks lived as `benchmarks/latency/mocks/*.py` (pure
stdlib sockets + 24-byte timestamp block). The C++ rewrite landed in
five phases recorded in the git log under `feat(mockex): Phase N`.
