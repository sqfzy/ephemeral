# Latency Benchmark Fairness

How the kernel-vs-DPDK latency comparison in `benchmarks/latency/` is structured so
that the numbers are apples-to-apples.

## The setup

`benchmarks/latency/` contains one `lat_<scenario>[_dpdk]` binary per test scenario:

| Scenario | Kernel client | DPDK client | Measures |
|---|---|---|---|
| `tcp` | `lat_tcp` | `lat_tcp_dpdk` | raw TCP round-trip latency |
| `udp` | `lat_udp` | `lat_udp_dpdk` | raw UDP round-trip latency |
| `ws`  | `lat_ws`  | `lat_ws_dpdk`  | plain WebSocket RTT (no TLS) |
| `ex_market` | `lat_ex_market` | `lat_ex_market_dpdk` | one-leg exchange bookTicker push |
| `ex_order`  | `lat_ex_order`  | `lat_ex_order_dpdk`  | N-inflight order RTT pipeline |
| `ex_md_udp` | `lat_ex_md_udp` | `lat_ex_md_udp_dpdk` | exchange UDP market data RTT |

Each binary:

1. **Forks a kernel-side mock echo server** (always kernel — same mock, same `recv()`
   and `send()` sequence, bound to NIC_A).
2. **Runs the bench client on the other side of the wire**. For `lat_*` that's a
   `KernelTcpStream` / `KernelUdpSocket` over NIC_B. For `lat_*_dpdk` that's a
   `DpdkTcpStream` / `DpdkUdpSocket` over the same NIC_B bound to vfio-pci.
3. **Reports a 4-leg TSC-nanosecond breakdown** (RTT / TX / RX / SRV) from four
   timestamps stamped into the payload: `client_send`, `server_recv`, `server_send`,
   `client_recv`.
4. **Writes no files** — everything goes to stdout.

Both client paths share identical config, identical codec (`WsCodec` / `RawStreamCodec`),
identical histogram infrastructure (`eph::utils::HdrHistogram`), identical TSC
calibration (`eph::utils::TSC`). The *only* thing that changes between `lat_tcp` and
`lat_tcp_dpdk` is which namespace's `TcpStream` is instantiated.

## Why forking a kernel mock is the fair choice

Alternatives considered and rejected:

| Alternative | Problem |
|---|---|
| Use a DPDK mock for DPDK runs | Would need a full DPDK echo server; small bugs there can masquerade as client-side wins or losses. Also doubles the test surface. |
| Use two DPDK clients (loopback inside one EAL) | DPDK loopback is not realistic — no PCIe, no NIC DMA, no flow-steering path. |
| Round-trip to a real exchange | Network jitter dominates; can't isolate kernel-vs-DPDK cost of the stack. |

The fair move is: **always use the same kernel mock as the "server"**. The mock's
latency contribution is identical across both client paths, so when we subtract the
`SRV` leg (`server_send - server_recv`) from the RTT, the *difference* between
`lat_tcp` and `lat_tcp_dpdk` numbers is exactly the difference between the kernel and
DPDK client paths. The per-leg `TX` (`server_recv - client_send`) and
`RX` (`client_recv - server_send`) legs let you see where in the client stack the
cost lives.

## Clock domains

All four timestamps are `eph::utils::TSC::now()` — a calibrated `rdtsc`-based clock.
The client and server run on the same box (mock is forked as a child process), so
TSC is coherent across cores on modern x86 (`invariant_tsc` CPUID bit) and ARM
(virtual counter). No cross-clock conversion is needed.

The bench config in `benchmarks/latency/bench.conf` pins the client and server to
separate isolated cores so they don't fight for the same CPU.

## The wrapper

Running the bench by hand requires transitioning NIC_B between three states:

- `host kernel` — bound to ENA, visible in `ip link`
- `bench_ns` — moved into a dedicated namespace for the kernel client
- `vfio-pci` — bound to the DPDK driver for the DPDK client

The wrapper script `benchmarks/latency/lat` handles all three transitions idempotently:

```bash
sudo ./benchmarks/latency/lat tcp           # host kernel client
sudo ./benchmarks/latency/lat tcp --dpdk    # transitions to vfio-pci, runs DPDK client
sudo ./benchmarks/latency/lat ex_market     # exchange scenario
```

You can run `lat` on any host — if vfio-pci isn't available (no IOMMU, or the NIC
isn't supported), the script prints a diagnostic and exits cleanly. The actual
`lat_*` binaries also skip gracefully when the required NIC state is missing.

## Verification

`tests/unit/bench/` contains unit tests for the shared bench infrastructure
(`core/runner.hpp`, `core/tsc_protocol.hpp`, `core/histogram_io.hpp`). Running
`xmake run -g tests` exercises them on every CI build.

`benchmarks/latency/core/runner.hpp` is the single source of truth for "where in the
code we take each of the four timestamps." Both kernel and DPDK clients call into the
same runner template, so the four measurement points are physically the same
instructions — only the underlying `Stream` / `Datagram` implementation differs.

## See also

- `benchmarks/latency/core/tsc_protocol.hpp` — the 4-timestamp payload layout
- `benchmarks/latency/core/runner.hpp` — the shared bench loop
- `benchmarks/latency/bench.conf` — NIC / IP / CPU / namespace config
- `docs/dpdk-setup.md` — how to prepare NIC_B for DPDK binding
