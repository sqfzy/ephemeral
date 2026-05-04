# eph-net fuzzers

libFuzzer harnesses for the zero-heap parsers under `eph/net/`. Run
periodically to stress untrusted-network entry points with crafted byte
streams.

Currently shipped:

| Harness                  | Target                                            | Seed corpus                                |
|--------------------------|---------------------------------------------------|--------------------------------------------|
| `fuzz_http_response.cpp` | `eph::net::parse_http_response` (RFC 7230 subset) | `corpus/fuzz_http_response/` (10 inputs)   |

## Why fuzz the HTTP response parser

`HttpClient` (used for REST trading APIs and the WebSocket upgrade
handshake) feeds whatever bytes a peer returns straight into
`parse_http_response`. A hostile or buggy reverse proxy / CDN /
intermediate can craft any status line, header block, or body
segment — the parser must be robust against arbitrary streams. ASan +
UBSan in the harness catch out-of-bounds reads, unsigned-overflow
arithmetic on `Content-Length`, and any signed-overflow in the status
code or header offsets.

## Requirements

- Clang ≥ 17 with libFuzzer (`-fsanitize=fuzzer` support)
- spdlog headers + the project include paths

The project's default toolchain is GCC 14, which has no libFuzzer.
The fuzzers are therefore intentionally outside the `xmake build -g
tests` graph.

## Build

```bash
# From the repo root:
clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
    -Ieph-net/include \
    -Ieph-core/include \
    -Ieph-utils/include \
    -lspdlog \
    eph-net/fuzzers/fuzz_http_response.cpp \
    -o fuzz_http_response
```

## Run

```bash
mkdir -p /tmp/http_resp_fuzz
cp eph-net/fuzzers/corpus/fuzz_http_response/* /tmp/http_resp_fuzz/

# Short bake (1 min)
./fuzz_http_response -max_total_time=60 -max_len=8192 /tmp/http_resp_fuzz

# Extended bake (10 min, stop on first finding)
./fuzz_http_response -max_total_time=600 -max_len=8192 \
    -error_exitcode=77 /tmp/http_resp_fuzz
```

## Seed corpus

The 10 binary inputs in `corpus/fuzz_http_response/` cover:

| Seed              | What it exercises                                          |
|-------------------|------------------------------------------------------------|
| `seed_ok`         | Vanilla 200 OK, Content-Length: 0                          |
| `seed_204`        | 204 No Content (bodyless status, no CL needed)             |
| `seed_101_ws`     | 101 Switching Protocols (the WS handshake response)        |
| `seed_500_body`   | 500 with non-empty body — exercises CL → body framing      |
| `seed_no_reason`  | "HTTP/1.1 200" with no SP reason phrase                    |
| `seed_http10`     | HTTP/1.0 with body                                         |
| `seed_runt`       | Truncated start-line (parser must say "need more bytes")   |
| `seed_garbage`    | Pure non-ASCII bytes                                       |
| `seed_huge_cl`    | Content-Length = 2^32-1 (overflow / OOB read attack)       |
| `seed_te_chunked` | Transfer-Encoding chunked (rejected by design)             |

When libFuzzer finds a crash / sanitizer hit, it writes the minimized
input to `crash-<sha1>` in the current directory. Copy that file into
`corpus/fuzz_http_response/` as a regression seed after fixing the bug.

## Adding a new harness

Same convention as `eph-net-dpdk/fuzzers/`: drop `fuzz_<target>.cpp`
here, add `corpus/fuzz_<target>/` with 3-10 seeds covering well-formed
/ boundary / malformed cases, and update the table above.
