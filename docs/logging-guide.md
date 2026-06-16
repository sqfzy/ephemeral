# Logging Guide

eph is a library, so it follows the cardinal rule of library logging: **it does
not decide where your application's output goes.** By default eph emits nothing
at all — no lines on your stdout, nothing in your spdlog default logger, no
loggers or sinks registered, and zero hot-path cost. You opt in explicitly when
you want diagnostics.

## TL;DR

| You want… | Do this |
|-----------|---------|
| Production (default) | nothing — eph is silent and compiles its log sites out |
| Turn eph logging on (this repo's builds) | `xmake f --eph_log=y && xmake` |
| Turn it on (your own build) | compile with `-DEPH_ENABLE_LOG` |
| Tune verbosity | also define `-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG` (TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL/OFF) |
| See only one subsystem | filter on the logger name, all under `eph.*` (e.g. `eph.net.dpdk.tcp`) |

## Design: a single compile-time gate

All eph logging flows through one header — `eph/core/log.hpp` — which provides
the `EPH_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL(logger, …)` macro family and
the `eph::log::get(name)` logger factory. The whole library logs exclusively
through these; it never calls bare `SPDLOG_*` (which target spdlog's *default*
logger) and never opens a sink on its own initiative.

The gate is the single macro `EPH_ENABLE_LOG`:

- **Undefined (default):** every `EPH_LOG_*` expands to a no-op. Arguments are
  not evaluated, so there is no formatting cost and no accidental side effects;
  no logger is created and no sink is opened. This holds **regardless of include
  order** — it does not matter whether your application includes `<spdlog/…>`
  before or after eph, and eph never mutates your own `SPDLOG_ACTIVE_LEVEL`.
- **Defined:** `EPH_LOG_*` forwards to spdlog's `SPDLOG_LOGGER_*`, writing to a
  per-subsystem named logger on stdout.

Because the gate is `EPH_ENABLE_LOG` (not `SPDLOG_ACTIVE_LEVEL`), enabling eph
logging is decoupled from how you configure your own spdlog usage.

## Logger names

When enabled, every eph subsystem logs through its own named logger, all under
the `eph.` namespace, so the source of each line is visible (spdlog's default
pattern includes `%n`) and you can filter the whole library at once:

```
[eph.net.dpdk.tcp]      … connection established …
[eph.fix.parser]        … message decoded …
[eph.codec.ws]          … pong sent …
```

Names follow `eph.<module>.<component>`, e.g. `eph.net.dpdk.tcp`,
`eph.net.kernel.byte_socket`, `eph.fix.position`, `eph.codec.raw_stream`.

## Tuning verbosity

When enabled and you have not set it yourself, `SPDLOG_ACTIVE_LEVEL` defaults to
the most verbose level so `EPH_ENABLE_LOG` alone controls output. Narrow it by
predefining `SPDLOG_ACTIVE_LEVEL` before any eph header (or via the build):

```bash
# this repo
xmake f --eph_log=y
xmake f --cxflags="-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN"

# your project
g++ -DEPH_ENABLE_LOG -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO …
```

Levels below `SPDLOG_ACTIVE_LEVEL` are compiled out entirely.

## Redirecting eph logs

When enabled, eph loggers are plain spdlog loggers registered under their `eph.*`
names and created lazily on first use. To send them somewhere other than stdout,
reconfigure them through the spdlog registry from your application, e.g.:

```cpp
spdlog::apply_all([](std::shared_ptr<spdlog::logger> lg) {
    if (lg->name().starts_with("eph.")) {
        lg->set_level(spdlog::level::warn);
        // lg->sinks() = { your_sink };  // redirect to file/journald/etc.
    }
});
```

(eph deliberately ships no runtime sink-injection API: in its target domain —
ultra-low-latency HFT — production runs with logging compiled out, and debug
builds want stdout. If you need richer routing, configure the `eph.*` loggers
as above.)

## Writing log statements inside eph

```cpp
#include "eph/core/log.hpp"

namespace eph::mymod::detail {
inline spdlog::logger* mymod_logger() {
    static spdlog::logger* l = ::eph::log::get("mymod.thing");  // registers "eph.mymod.thing"
    return l;
}
} // namespace eph::mymod::detail

// at a call site, pass the logger explicitly — never a bare SPDLOG_* macro:
EPH_LOG_DEBUG(detail::mymod_logger(), "did X with n={}", n);
```

Rules for contributors:

- Include `eph/core/log.hpp`, never `<spdlog/spdlog.h>` directly.
- Use `EPH_LOG_*` with an explicit subsystem logger — never bare `SPDLOG_*`
  (those hit the host's default logger) and never `spdlog::info(...)` free calls
  (those hit the default logger at runtime, ungated).
- Get the logger via `eph::log::get("<module>.<component>")`.
- Disabled log calls are still type-checked, so keep format strings and
  arguments valid even for `TRACE`/`DEBUG`.
