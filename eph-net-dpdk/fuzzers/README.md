# eph-net-dpdk fuzzers

libFuzzer harnesses for the zero-heap parsers under `eph/dpdk/`. Run
periodically (CI gate, pre-release bake, or ad-hoc) to stress the
parsers with crafted byte streams.

Currently shipped:

| Harness               | Target                                                        | Seed corpus                            |
|-----------------------|---------------------------------------------------------------|----------------------------------------|
| `fuzz_dns_reply.cpp`  | `eph::dpdk::dns::detail::parse_dns_response` + `skip_dns_name` | `corpus/fuzz_dns_reply/` (8 inputs)   |
| `fuzz_arp_reply.cpp`  | `eph::dpdk::arp::parse_arp_reply`                              | `corpus/fuzz_arp_reply/` (10 inputs)  |
| `fuzz_icmp_reply.cpp` | `eph::dpdk::net::parse_icmp` + `parse_ip_header` + `is_ip_fragment` | `corpus/fuzz_icmp_reply/` (15 inputs) |
| `fuzz_udp_packet.cpp` | `eph::dpdk::net::parse_udp_packet` + layered `parse_udp_from_ip` / `parse_tcp_from_ip` | `corpus/fuzz_udp_packet/` (11 inputs) |

## Requirements

- Clang ≥ 17 with libFuzzer (`-fsanitize=fuzzer` support)
- spdlog + the same include paths the project uses for release builds

The harnesses themselves do NOT pull in any TLS library: the targeted
parsers (`dns.hpp`, `arp.hpp`, `packet_parse.hpp`) deliberately route
around aws-lc / openssl — `dns.hpp` uses `getrandom(2)` for tx-id
generation precisely to keep the TU clean of `<openssl/rand.h>`.

The project's default toolchain is GCC 14, which has no libFuzzer. The
fuzzers are therefore **intentionally not wired into the xmake default
build** — they live outside the `xmake build -g tests` graph and must
be invoked with clang directly.

## Build

The `fuzz_dns_reply` and `fuzz_arp_reply` harnesses shim the DPDK
primitives they need, so they build against the project include paths
alone:

```bash
# From the repo root:
clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
    -Ieph-net-dpdk/include \
    -Ieph-core/include \
    -Ieph-utils/include \
    -lspdlog \
    eph-net-dpdk/fuzzers/fuzz_dns_reply.cpp \
    -o fuzz_dns_reply
```

The `fuzz_icmp_reply` and `fuzz_udp_packet` harnesses target
`packet_parse.hpp`, which pulls in real DPDK mbuf / ether / ip struct
definitions. They therefore build against system libdpdk headers
(the fuzzer never calls `rte_eal_init` — only struct definitions and
inline accessors are exercised):

```bash
# From the repo root:
clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
    -Ieph-net-dpdk/include \
    -Ieph-core/include \
    -Ieph-utils/include \
    $(pkg-config --cflags libdpdk) \
    -lspdlog \
    eph-net-dpdk/fuzzers/fuzz_icmp_reply.cpp \
    -o fuzz_icmp_reply

clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
    -Ieph-net-dpdk/include \
    -Ieph-core/include \
    -Ieph-utils/include \
    $(pkg-config --cflags libdpdk) \
    -lspdlog \
    eph-net-dpdk/fuzzers/fuzz_udp_packet.cpp \
    -o fuzz_udp_packet
```

## Seed corpus

`corpus/fuzz_dns_reply/` contains 8 tiny binary inputs that cover the
known edge classes — well-formed answer, empty, 1-byte runt, tx-id-only,
header-only, count-field overflow, pointer loop, malformed label length.
libFuzzer uses these as starting points for its evolutionary mutation;
diverse seeds materially accelerate discovery of new code paths.

When adding a new interesting failing case, drop the file into the
corpus directory and commit it — the corpus is version-controlled.

## Run

```bash
# Copy seeds to a mutable working directory (libFuzzer writes new
# discoveries into the corpus dir; keep the repo corpus read-only).
mkdir -p /tmp/dns_fuzz_corpus
cp eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/* /tmp/dns_fuzz_corpus/

# Short bake (1 min)
./fuzz_dns_reply -max_total_time=60 -max_len=512 /tmp/dns_fuzz_corpus

# Extended bake (10 min, ASan+UBSan, stop on the first finding)
./fuzz_dns_reply -max_total_time=600 -max_len=512 \
    -error_exitcode=77 /tmp/dns_fuzz_corpus
```

If libFuzzer triggers a crash or sanitizer finding, it writes the
minimized input to `crash-<sha1>` in the current directory. Copy that
file into `corpus/fuzz_dns_reply/` as a regression seed after fixing
the bug.

## Adding a new harness

1. Drop `fuzz_<target>.cpp` in this directory.
2. Create `corpus/fuzz_<target>/` with 3-10 seed inputs covering
   well-formed / boundary / malformed cases.
3. Add a row to the table above and describe the target function.
4. Harnesses must return 0 from `LLVMFuzzerTestOneInput` — crashes /
   sanitizer hits are the only reportable signal.
