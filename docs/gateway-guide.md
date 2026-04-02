# Gateway Guide

Multi-connection lifecycle manager for coordinated Transport management.

## Overview

`Gateway` orchestrates multiple `Transport` instances (e.g., simultaneous connections to multiple exchanges/trading pairs), providing centralized start/stop/reconnect control and background health monitoring.

Header: `eph/net/gateway.hpp`  
Namespace: `eph::net`

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Gateway                                    │
│                                                                     │
│  ┌───────────┐  ┌──────────────────────────────────────────────┐   │
│  │  Config    │  │  connections_: vector<GatewayConnection>     │   │
│  │           │  │                                              │   │
│  │ interval  │  │  ┌────────────────┐  ┌────────────────┐     │   │
│  │ threshold │  │  │ [0] binance-btc│  │ [1] okx-eth    │ ... │   │
│  │ callback  │  │  │                │  │                │     │   │
│  └───────────┘  │  │ void* ─────────┼──► Transport<T1>  │     │   │
│                 │  │ stop_fn()      │  │ stop_fn()      │     │   │
│  ┌───────────┐  │  │ is_running_fn()│  │ is_running_fn()│     │   │
│  │  mu_      │  │  │ start_fn()    │  │ start_fn()     │     │   │
│  │ (mutex)   │  │  │ reconnect_fn()│  │ reconnect_fn() │     │   │
│  └───────────┘  │  │ health: ●     │  │ health: ●      │     │   │
│                 │  │ priority: 1   │  │ priority: 128  │     │   │
│                 │  └────────────────┘  └────────────────┘     │   │
│                 └──────────────────────────────────────────────┘   │
│                                                                     │
│  ┌──────────────────────────────────────┐                          │
│  │  monitor_thread_                      │                          │
│  │  ┌────────────────────────────────┐  │                          │
│  │  │ while (monitor_running_) {     │  │                          │
│  │  │   check_health();             │  │                          │
│  │  │   sleep 100ms × N chunks;     │◄─── atomic<bool>            │
│  │  │ }                             │  │  monitor_running_         │
│  │  └────────────────────────────────┘  │                          │
│  └──────────────────────────────────────┘                          │
└─────────────────────────────────────────────────────────────────────┘
```

## Type Erasure

Gateway uses `void*` + function pointers to manage any Transport type without templates leaking into the container:

```
  add<Transport>("tag", &tp)
         │
         ▼
  ┌──────────────────────────────────────────┐
  │ GatewayConnection                        │
  │                                          │
  │ transport_ptr = (void*)&tp               │
  │                                          │
  │ stop_fn = [](void* p) {                  │
  │     static_cast<Transport*>(p)->stop();  │
  │ };                 ▲                     │
  │                    │ capture type at      │
  │ is_running_fn = ...│ compile time,        │
  │ start_fn = ...     │ call uniformly at    │
  │ reconnect_fn = ... │ runtime              │
  └──────────────────────────────────────────┘
```

The concrete type is captured once in `add()` via lambda closures. After that, Gateway operates on all connections through the uniform function pointer interface.

## Health State Machine

```
                    start_all()
   ┌─────────┐ ──────────────────► ┌─────────┐
   │ Stopped │                     │ Healthy │◄────────┐
   └─────────┘ ◄──────────────── ─ └─────────┘         │
        ▲          stop_all()          │                │
        │                              │ check_health() │ check_health()
        │                              │ !is_running()  │ is_running()
        │                              ▼                │
        │                        ┌──────────────┐      │
        └─────── stop_all() ──── │ Disconnected │──────┘
                                 └──────────────┘

        ┌──────────┐
        │ Degraded │  (reserved, not yet implemented)
        └──────────┘
```

- **Stopped**: Intentionally stopped, not monitored.
- **Healthy**: Connected and `is_running()` returns true.
- **Disconnected**: `is_running()` returned false during a health check.
- **Degraded**: Reserved for future use — will require per-connection last-activity timestamps.

## Lifecycle Sequence

```
  User              Gateway               Transport[]        Monitor
   │                   │                      │                 │
   │── add(tag,tp) ──►│                      │                 │
   │                   │── type-erase+store ─►│                 │
   │◄── return id ─────│                      │                 │
   │                   │                      │                 │
   │── start_all() ──►│                      │                 │
   │                   │── start_threads() ──►│                 │
   │                   │── health=Healthy     │                 │
   │                   │                      │                 │
   │── start_monitor()►│                      │                 │
   │                   │──────── spawn ────────────────────────►│
   │                   │                      │                 │
   │                   │                      │    ┌──── loop ──┤
   │                   │                      │    │            │
   │                   │◄── check_health() ───────── is_running?│
   │                   │                      │──► bool ────────│
   │                   │                      │    │            │
   │                   │   on_health_change() │    │ sleep 5s   │
   │◄─ callback ───────│  (if state changed)  │    │ (100ms×50) │
   │                   │                      │    └────────────┤
   │                   │                      │                 │
   │── stop_monitor()─►│                      │                 │
   │                   │── running_=false ─────────────────────►│
   │                   │◄──────── join ────────────────────────►│ exit
   │                   │                      │
   │── stop_all() ───►│                      │
   │                   │──── stop() ─────────►│
   │                   │── health=Stopped     │
   │                   │                      │
```

## Thread Safety

- All public methods hold `mu_` (std::mutex).
- `monitor_running_` uses `atomic<bool>` with acquire/release ordering to coordinate the monitor thread's start/stop.
- Monitor sleep is split into 100ms chunks for responsive shutdown — checks `monitor_running_` between each chunk.

## Usage

```cpp
#include <eph/net/gateway.hpp>

// Configure
eph::net::Gateway::Config config;
config.health_check_interval = std::chrono::milliseconds{5000};
config.on_health_change = [](std::string_view tag, ConnHealth old_h, ConnHealth new_h) {
    spdlog::warn("Connection '{}' health: {} -> {}", tag,
                 conn_health_name(old_h), conn_health_name(new_h));
};

eph::net::Gateway gw(config);

// Register connections
auto id1 = gw.add("binance-btc", &transport1, /*priority=*/1);
auto id2 = gw.add("okx-eth", &transport2);

// Start
gw.start_all();
gw.start_monitor();

// Runtime inspection
std::cout << gw.dump();

// Force reconnect a specific connection
gw.reconnect(id1);

// Shutdown
gw.stop_monitor();
gw.stop_all();
```
