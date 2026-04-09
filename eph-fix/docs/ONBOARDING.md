# eph-fix Developer Onboarding

Welcome. This guide gets a new contributor from a fresh clone to writing and
running tests against `eph-fix` in under fifteen minutes. It assumes you have
never opened this subproject before.

## Development Environment

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| C++23 compiler | GCC 13+ or Clang 17+ | `std::expected`, `std::format`, `std::span`, `[[nodiscard]]`, concepts |
| [xmake](https://xmake.io) | 2.8+ | The monorepo's single source of build truth |
| Python | 3.8+ | Only required by some tooling scripts in the parent repo |
| Git | any recent | |

Runtime/compile dependencies are declared in the parent `xmake.lua` and are
fetched automatically on first build:

- `spdlog` — every module owns a named logger
- `gtest` — unit tests
- `eph-core` (sibling subproject) — `eph::core::parse_int`, `parse_number`, and
  the `eph::net::MessageFramer` concept

### First build

From the monorepo root (`/home/ec2-user/ephemeral_dev` in this clone):

```bash
xmake f -m release          # configure: release mode, ccache on by default
xmake build eph-fix         # header-only target; nothing to link, but validates the headers compile cleanly via the tests
xmake build test_fix        # main test binary (~330 test cases)
xmake run   test_fix        # run it
```

A complete smoke check across the whole subproject:

```bash
for t in test_fix test_fix_session test_fix_orders test_execution_report \
         test_order_manager test_position test_risk_check; do
    xmake build "$t" && xmake run "$t" || exit 1
done
```

### Sanitizer builds

The top-level `xmake.lua` defines `asan` and `tsan` modes:

```bash
xmake f -m asan && xmake build test_fix && xmake run test_fix
xmake f -m tsan && xmake build test_fix_session && xmake run test_fix_session
```

`test_fix_session` is the right place to catch data races because `FixSession`
is the only component intentionally shared across threads.

## Architecture at a Glance

`eph-fix` is layered from the wire up:

```
                      ┌─────────────────────────────┐
                      │       Application code      │
                      └─────────────┬───────────────┘
                                    │
                  ┌─────────────────┴──────────────────┐
                  │                                    │
          ┌───────▼──────┐                    ┌────────▼────────┐
          │ OrderManager │<────fills──────────│ PositionTracker │
          └───────┬──────┘                    └────────┬────────┘
                  │                                    │
                  │                           ┌────────▼────────┐
                  │                           │   RiskChecker   │
                  │                           └─────────────────┘
                  │
         ┌────────▼─────────┐     ┌──────────────┐
         │ExecutionReport   │     │   orders.hpp │
         │      View        │     │  build_*()   │
         └────────┬─────────┘     └──────┬───────┘
                  │                      │
                  └──────────┬───────────┘
                             │
                  ┌──────────▼──────────┐
                  │    FixSession       │  Logon/Logout/HB/SeqNum
                  └──────────┬──────────┘
                             │
           ┌─────────────────┼──────────────────┐
           │                 │                  │
     ┌─────▼─────┐    ┌──────▼──────┐   ┌───────▼────────┐
     │  parser   │    │  builder    │   │    framer      │
     │  (view)   │    │  (bytes)    │   │ (msg boundary) │
     └─────┬─────┘    └──────┬──────┘   └───────┬────────┘
           │                 │                  │
           └─────────────────┴──────────────────┘
                             │
                       tags.hpp (constants, names)
```

A typical RX flow:

1. Transport delivers raw bytes → `FixFramer::decode()` identifies a complete
   message.
2. `FixSession::on_rx()` runs `parse()`, handles session-level messages
   (Heartbeat, Logon, Logout, SequenceReset, ResendRequest) internally, and
   returns `false` if this is an application message.
3. Application parses further (e.g. `try_parse_execution_report()`) and
   feeds the result to `OrderManager::on_execution_report()`.
4. `OrderManager` updates order state and (optionally) forwards fills to a
   `PositionTracker`.

A typical TX flow:

1. Application builds a `MessageBuilder` with the application-level fields.
2. `FixSession::send_app(builder)` fills `SenderCompID`, `TargetCompID`,
   `MsgSeqNum`, `SendingTime`, calls `finish()`, and hands the bytes to the
   user-provided `SendFn`.

### Directory layout

```
eph-fix/
├── include/eph/fix/   # public headers — all code lives here
├── tests/             # one .cpp per module, auto-picked by xmake.lua
├── benchmarks/        # bench_fix_parse.cpp (parser throughput)
└── xmake.lua          # header-only target + test/bench discovery
```

### Key entry points

- `eph::fix::parse(data, len)` — turn wire bytes into `BasicMessageView`.
- `eph::fix::MessageBuilder` — produce wire bytes from structured data.
- `eph::fix::FixFramer` — plug into `eph-transport` to framed wire I/O.
- `eph::fix::FixSession` — session lifecycle and heartbeat.
- `eph::fix::try_parse_execution_report()` — one-call wire → typed view.
- `eph::fix::OrderManager::on_execution_report()` — state-machine update
  plus optional position forwarding.

## Everyday Development

### Build

```bash
xmake build <target>          # e.g. test_fix, test_fix_session, bench_fix_parse
xmake build -a                # build everything
```

### Test

All tests use GoogleTest. They are plain executables:

```bash
xmake run test_fix --gtest_filter='FixParser.*'
xmake run test_fix_session --gtest_filter='*Logon*'
```

### Benchmark

```bash
xmake run bench_fix_parse
```

The conventional flow for performance work on this subproject is:

1. Run `bench_fix_parse` on `main` to capture a baseline.
2. Make the change.
3. Run `bench_fix_parse` again and confirm no regression before committing.
4. If a regression shows up, investigate before shipping — the repo-wide
   CLAUDE.md treats regressions as blockers.

### Add a new public API

1. Add the declaration/definition under `include/eph/fix/<feature>.hpp`.
2. Add a Doxygen-style `///` block summarizing purpose, params, return value,
   and any precondition / postcondition that is not obvious from the type.
3. Add a test file `tests/test_<feature>.cpp` — `xmake.lua` picks it up
   automatically.
4. If the component is user-facing, add a short usage snippet to README.md
   and note the addition in CHANGELOG.md.
5. Ensure tests cover error paths, not only the happy path.

### Debug a failing test

```bash
xmake f -m debug && xmake build test_fix
gdb --args $(xmake ls -f test_fix) --gtest_filter='FixParser.broken_case'
```

Turn on trace logging at compile time by building with
`-DSPDLOG_ACTIVE_LEVEL=0` (TRACE). The parent `xmake.lua` already reads a
`net_log_level` project variable.

## Code Conventions

### Naming

- Types: `UpperCamelCase` (`MessageBuilder`, `FixSession`, `OrderManager`).
- Functions and methods: `snake_case` (`parse`, `get_int`, `on_execution_report`).
- Enum members: `UpperCamelCase` for FIX-protocol enums carrying semantic
  character values (`Side::Buy`, `ExecType::Fill`); `kPrefixed` for internal
  state/error enums (`ParseError::kIncomplete`, `SessionState::kActive`).
- Namespaces: `snake_case` (`eph::fix::tag::msg_type`).
- Private members: trailing underscore (`state_`, `outbound_seq_`).
- Constants: `kSnakeCase` (`kDefaultMaxBodyLength`, `kHeaderReserve`).

### Error handling

- Parsers and builders return `std::expected<T, Error>` or `std::optional<T>`.
- Never throw in hot paths.
- Sessions return `std::expected<void, std::string>` from `logon()` /
  `logout()` so the caller gets an actionable error message.
- Log **and** return on failure — never swallow errors silently.

### Logging

- One named logger per module, obtained via a `detail::<module>_logger()`
  helper that lazily creates the logger on first use. The naming scheme is
  `fix.<module>` (e.g. `fix.parser`, `fix.session`, `fix.ordmgr`).
- Use the `SPDLOG_LOGGER_*` compile-time macros so disabled levels compile
  away to nothing.
- Log messages must be actionable: include relevant context — tag numbers,
  `cl_ord_id`, sequence numbers, offsets, state names.

### Commits

The repo uses Conventional Commits. Scope the change to `fix` (or
`fix,itch`, `fix,core`, etc. when it touches multiple subprojects). Examples
drawn from recent history:

- `feat(fix): add order lifecycle manager (OMS core)`
- `fix(fix): prevent UB in set_raw() with null data`
- `refactor(fix): code review fixes — overflow guards and constexpr consistency`
- `chore(fix): remove unused <limits> include after parse_int extraction`

Keep each commit buildable and focused on one logical change.

## Frequently Asked Questions

**Q: The build fails with "undefined reference to spdlog::...".**
A: Make sure you build from the monorepo root so `xmake` picks up the
`spdlog` package fetched by the parent `xmake.lua`. Do not run `xmake`
directly inside `eph-fix/` — it is a sub-target, not a standalone project.

**Q: My fields show up out of order.**
A: FIX preserves insertion order inside the body; check that you are calling
`set()` in the FIX-spec-required order (header tags first, then application
tags, then repeating groups at the bottom).

**Q: `MessageBuilder::finish()` returns 0.**
A: The buffer overflowed or you set a non-finite double or embedded SOH.
Enable DEBUG logging on `fix.builder` — every overflow/rejection path logs a
detailed reason.

**Q: Session keeps disconnecting after ~45 seconds of silence.**
A: That's `heartbeat_timeout_factor` (default 1.5×) kicking in after the
configured `heartbeat_interval_sec`. Either send heartbeats more often or
raise the factor via `FixSessionConfig`.

**Q: `parse()` returns `kIncomplete` even though the buffer looks full.**
A: FIX messages are self-delimiting by `BodyLength` + `10=XXX\x01`. If the
trailing SOH of the CheckSum is missing, `parse()` considers the frame
incomplete. Confirm the sender actually terminated the message.

**Q: Where do I put a new FIX tag constant?**
A: `include/eph/fix/tags.hpp`, grouped by section (session / order entry /
market data). Add it to `tag_name()` if you also want it to show up in
`dump()`/`to_json()` output.
