# eph-containers — Developer Onboarding

Welcome. This guide is for new contributors who need to be productive
on the `eph-containers` sub-project within a day. It covers
environment setup, the five-minute architecture tour, the standard
build/test loop, and the conventions you must follow when adding code
or tests.

## 1. Development environment

### Prerequisites

| Tool            | Version                         | Notes |
|-----------------|---------------------------------|-------|
| C++ toolchain   | GCC 14+ or Clang with C++23     | `std::format`, `std::span`, concepts, `std::has_single_bit`, `std::construct_at` are all required. |
| xmake           | current stable                  | Build system for the whole `ephemeral_dev` monorepo. |
| Python `uv`     | optional                        | Only needed for auxiliary scripts; not required to build this sub-project. |
| `git`           | any modern                      | Conventional-commits style is used throughout. |

On Amazon Linux 2023 (the primary dev box), GCC 14 is available as
`gcc14-g++`; xmake will pick it up automatically via the toolchain
detection. On WSL Arch Linux, `pacman -S gcc xmake` is sufficient.

The test and benchmark binaries pull **gtest**, **gmock**,
**benchmark**, **spdlog**, and **tabulate** through xmake package
management — no manual installation required. The first `xmake build`
will populate `~/.xmake/packages/` with these dependencies.

### Clone and first build

```bash
git clone <repo-url> ephemeral_dev
cd ephemeral_dev

# Build just the eph-containers sub-project headers (headeronly target).
xmake build -g eph-containers

# Build an individual test or benchmark binary:
xmake build test_ring_buffer
xmake build bench_bq_pingpong
```

### Verifying your environment

```bash
# Run every eph-containers test binary. Should report 434 passing tests
# across 6 binaries (45 + 15 + 143 + 37 + 139 + 55).
for t in test_concepts test_ring_buffer test_bounded_queue \
         test_bounded_queue_bytes test_evicting_queue \
         test_evicting_queue_bytes; do
    xmake run "$t" || { echo "FAIL: $t"; exit 1; }
done
```

If any test fails on a clean checkout, stop and ask in chat — this
library is concurrency-heavy and silent regressions are possible on
weakly-ordered architectures (aarch64).

## 2. Architecture in five minutes

`eph-containers` is a header-only library. Everything lives under
`include/eph/` — `xmake.lua` simply installs these headers and wires
up tests/benchmarks. There are **no compiled .cpp sources** in the
library itself.

Two queue families, two byte wrappers, one lookback buffer, one
concept:

```
  eph/containers.hpp  (umbrella)
  +-- concepts.hpp             -> TrivialData<T>
  +-- ring_buffer.hpp          -> RingBuffer<T, N>
  +-- bounded_queue.hpp        -> BoundedQueue<T, N>      + Stats
  +-- bounded_queue_bytes.hpp  -> BoundedQueueBytes<Max, N> (wraps BoundedQueue<DataWrap, N>)
  +-- evicting_queue.hpp       -> EvictingQueue<T, N> + <T, 1> specialisation + Stats
  +-- evicting_queue_bytes.hpp -> EvictingQueueBytes<Max, N> (wraps EvictingQueue<DataWrap, N>)
```

Mental model:

- **Bounded** = "reliable". Writes apply back-pressure; no data loss.
- **Evicting** = "latest-value wins". Writes are wait-free; old data
  is silently overwritten. Readers use optimistic SeqLock reads.
- **RingBuffer** = "history lookback". One writer, any number of
  readers, `at(offset)` returns the element `offset` steps behind
  the newest.

All queues are SPSC: exactly one thread calls the writer API, exactly
one thread calls the reader API. Monitoring methods (`stats()`,
`size()`, etc.) are safe from arbitrary threads but return
approximate values.

### Key entry points

- `BoundedQueue::try_produce(F)` / `try_consume(F)` — zero-copy visitor
  hot path. Everything else (`try_push`, `try_pop`, `push_n`, `pop_n`,
  `try_consume_all`, `*_for`) is a thin wrapper over these two.
- `EvictingQueue::produce(F)` / `try_consume_latest(F)` — likewise.
- `DataWrap` inside the `*_bytes` headers is the byte envelope struct
  (`{ id?, ts, len, data[MaxDataSize] }`). Read and understand it
  before touching either byte wrapper.

## 3. Day-to-day development

### Build

```bash
xmake build -g eph-containers              # header install target (fast)
xmake build test_bounded_queue             # one test binary
xmake build -a                             # everything in the workspace
```

### Test

```bash
xmake run test_bounded_queue               # single binary, GoogleTest
xmake run test_bounded_queue --gtest_filter='BoundedQueue*batch*'
```

All test binaries use GoogleTest + gmock and report on stdout. No
special env vars are required.

### Benchmark

```bash
xmake build bench_bq_pingpong
xmake run bench_bq_pingpong                # Google Benchmark output
```

Remember: **run benchmarks before AND after any performance-sensitive
change** and record both numbers. This is a project-wide rule; see
the global `CLAUDE.md` for the exact wording.

### Common tasks

#### Adding a new method to an existing queue

1. **Read** the `try_produce` / `try_consume` implementation for the
   queue you are extending and understand its memory-ordering
   invariants (where are the `acquire`/`release` barriers? where does
   the shadow index come from?).
2. **Add the new method** in the same style:
   - `[[nodiscard]]` on anything returning success/failure.
   - `noexcept` unless you genuinely need to throw (you don't).
   - `template <typename F> requires std::invocable<F, T&>` for
     visitor-based writers, `std::invocable<F, const T&>` for readers.
   - Prefer delegating to the visitor overload from the value overload
     (`try_pop(T&)` delegates to `try_consume([&](const T&) { ... })`).
3. **Add doxygen comments** — one-line brief, `@param`, `@return`,
   `@note` if there are thread-safety caveats, `@warning` if the
   method is not thread-safe.
4. **Write tests** covering:
   - The happy path.
   - The boundary cases (empty queue, full queue, `Capacity == 1`).
   - Concurrent producer/consumer if the new method interacts with the
     hot path.
5. **Run the test binary**: `xmake run test_<queue>`.
6. **Update the relevant benchmark** if the change affects hot-path
   throughput or latency. Measure before/after.

#### Adding a new test

Tests live in `tests/test_<header>.cpp`. The xmake `eph-test` rule
auto-discovers every `.cpp` in this directory, so you don't need to
touch `xmake.lua`. Use type-parameterised `::testing::Types` fixtures
if the same behaviour must hold for multiple capacities.

#### Debugging a concurrency bug

1. Rebuild with the TSan mode: `xmake f -m tsan && xmake build -g eph-containers && xmake build test_<name>`.
2. `xmake run test_<name>` — TSan will flag data races.
3. For ARM64-specific ordering bugs, read the relevant `produce` /
   `consume` function and check that every shared store uses
   `memory_order_release` and every matching load uses
   `memory_order_acquire`. Relaxed stores are only valid for
   writer-local state (shadow indices).

## 4. Code conventions

### Language style

- **C++23 only**. Use concepts, `std::format`, `std::span`,
  `std::expected`-style error returns, structured bindings, ranges.
- **Header-only**. Do not introduce `.cpp` files into `include/`.
- **Templates over type erasure**. Do not add `std::function` or
  virtual dispatch on the hot path.
- **No heap allocations** on the queue/ring-buffer hot path.
- Use `[[likely]]` / `[[unlikely]]` where profiling motivates it (e.g.
  `payload.size() > MaxDataSize [[unlikely]]`).

### Comments

- Public API gets doxygen `///` comments in the style shown in
  `bounded_queue.hpp`. Briefs end with a period. Use `@brief`,
  `@param`, `@return`, `@note`, `@warning`.
- Inline comments explain **why**, not **what**. If you write "Loop 3
  times", you are doing it wrong. If you write "Retry up to 3 times
  because the writer-side store fence must observe the load fence on
  ARM64", you are doing it right.

### Error handling

- Queues never throw. They return `bool` / `std::optional<T>` /
  `size_t (consumed)`. The blocking overloads (`push`, `pop`,
  `produce`, `consume`) assume success and spin forever.
- The only `noexcept(false)` spots are where `std::chrono::steady_clock::now()`
  might theoretically throw on an exotic platform; these are
  intentionally marked `noexcept` with a comment explaining the
  trade-off (you'll see it in every `*_for` function).

### Logging

- Runtime logging uses `SPDLOG_LOGGER_DEBUG` / `SPDLOG_LOGGER_WARN`
  with `SPDLOG_ACTIVE_LEVEL` for compile-time filtering.
- Named loggers only — call `detail::bounded_queue_logger()` or
  similar. Never use the root `spdlog` logger from library code.
- Messages must be **actionable**: include variable values and system
  state (`"bounded_queue stats clamping: tail={}, head={}, raw_size={}"`).

### Commit messages

Follow Conventional Commits:
`feat(containers): add X`, `fix(containers): handle Y`,
`perf(containers): Z`, `refactor(containers): …`,
`test(containers): …`, `docs(containers): …`, `chore: …`.

Each commit should compile and pass tests in isolation. Use
`xmake build -g eph-containers && xmake run test_<relevant>` before
committing.

## 5. Troubleshooting FAQ

### `fatal error: format: No such file or directory`
Your GCC is too old. Need GCC 14+ or recent Clang. On AL2023 use
`gcc14-g++`; on Arch `gcc` in current repos already supports C++23.

### `static assertion failed: Capacity must be a power of 2`
Capacity template parameter must be a power of two (1, 2, 4, 8, 16,
...). `std::has_single_bit` enforces this at compile time.

### `constraints not satisfied: TrivialData<T>`
Your `T` has a non-trivial destructor, copy constructor, or cannot be
default-initialised. SPSC queues require trivial copyability because
they `memcpy` into pre-allocated slots. Use a POD or add
`= default` equivalents.

### Test hangs forever
You are probably calling a blocking overload (`push`, `pop`,
`produce`, `consume`) from a thread that is also the sole
producer/consumer. Switch to the `try_*` family or use a dedicated
producer / consumer thread.

### Build succeeds, test segfaults
If you recently modified `EvictingQueue`, suspect a SeqLock memory
ordering bug first. Rebuild under TSan (`xmake f -m tsan`) and re-run
— TSan will localise the offending load/store pair.

### Stats counters don't match expected totals
`total_pushed - total_popped` is only exactly equal to `current_size`
in a quiescent system. During concurrent operation the counters are
monotonic but snapshots are approximate — the relaxed loads inside
`stats()` are deliberate to keep the call off the hot path.
