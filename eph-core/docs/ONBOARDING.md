# eph-core onboarding

## What's in this module

`eph-core` is the leaf dependency of the `eph-*` graph. It contains:

- **Core types** — `Error` / `ErrorInfo` enum, `StreamCodec` / `DatagramCodec`
  concepts, `OutputBuffer`, the `PacketView` contract.
- **Shared enums** — `TcpState` (RFC 793) is defined here so both kernel and DPDK
  backends can reference it without ODR conflicts.
- **Legacy framer primitives** — `MessageFramer` concept, `LengthPrefixFramer`,
  kept because `eph-fix`, `eph-itch`, `eph-json` still consume them and migrating
  the parsers would be a separate project.
- **Utility helpers** — `parse_number`, `parse_int`, `json_escape`, `base64_encode`,
  `contains_control_chars`.

## How to find things

| Question | File |
|---|---|
| What errors can I return from a codec? | `include/eph/core/error.hpp` |
| What's the codec concept shape? | `include/eph/core/codec.hpp` |
| What's the zero-copy contract? | `include/eph/core/packet_view.hpp` |
| What TCP states are legal? | `include/eph/core/tcp_state.hpp` |
| How do I parse exchange-format floats? | `include/eph/core/parse_number.hpp` |
| How do I get JSON-escaped output? | `include/eph/core/detail/json_escape.hpp` |

## Running the tests

```bash
xmake build -g tests
xmake run test_error
xmake run test_codec_concept
xmake run test_parse_number
xmake run test_length_prefix_framer
```

Per-file targets are auto-globbed from `tests/test_*.cpp`.

## Common tasks

### Adding a new error code

1. Add the enum value to `enum class Error` in `error.hpp`.
2. Add a `case` to `error_name()`.
3. Add a unit test in `tests/test_error.cpp`.

Do not add module-specific error codes here — the design uses one shared enum
for all networking errors, with a free-form `detail` string for context.

### Adding a new utility helper

Prefer `detail/` for anything that shouldn't be part of the public surface.
Header-only + `inline` or `template`, no `.cpp` files under `include/`.

## See also

- `README.md` — module overview
- `summary.md` — public API surface
- `CHANGELOG.md` — change history
- `../../docs/architecture.md` — the whole-project concept model
