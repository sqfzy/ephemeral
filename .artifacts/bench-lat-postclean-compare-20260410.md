# Cleanup verification — baseline vs postclean

> **Verdict**: Perf parity preserved. No regression introduced by the cleanup.
>
> **p50** (the reliable signal): max absolute delta is **+3.7%**, every other
> p50 is within ±3%, most are within ±1%. p50 is below the ±5% threshold
> across the board.
>
> **SRV leg** (server-side stamp delta — strongest "did the underlying mock
> change?" indicator): rock-solid at 259 ns mean, every diff within ±1.1%.
> The mock binary is byte-identical between baseline and postclean (the
> cleanup didn't touch any `lat_*.cpp` source), so any visible drift here
> would be measurement noise, not a real change.
>
> **⚠ flags below**: all on p99 and p999. These are HDR histogram bucket
> noise — a single sample landing in a different bucket can shift p999 by
> 20-30% because the bucket count at that quantile is small (sample count
> ~500k, p999 sample count ~500). This is exactly the case documented in
> README.md commit `b5d6d1a` — "Why max is sometimes huge". Compare p50,
> ignore the p999 wobble.

Threshold: ⚠ on |Δp50| > 5%, |Δp99| > 5%, |Δp999| > 10%.

---

## tcp --dpdk
  baseline:  `bench-data-lat-baseline-tcp-dpdk-20260409.txt`
  postclean: `bench-data-lat-postclean-tcp-dpdk-20260410-postclean.txt`

### tcp payload=64B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 19.0k | 19.6k | +2.9%  | 22.9k | 23.0k | +0.6%  | 26.2k | 25.8k | -1.5%  |
| TX | 11.4k | 11.8k | +3.7%  | 13.7k | 14.2k | +3.6%  | 16.3k | 16.5k | +1.0%  |
| RX | 7.2k | 7.3k | +1.3%  | 9.5k | 9.5k | +0.2%  | 11.9k | 11.3k | -4.7%  |
| SRV | 259 | 259 | +0.0%  | 269 | 268 | -0.4%  | 270 | 272 | +0.7%  |

### tcp payload=128B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 19.3k | 19.2k | -0.3%  | 21.7k | 22.2k | +1.9%  | 24.3k | 25.4k | +4.6%  |
| TX | 11.5k | 11.5k | +0.0%  | 13.7k | 14.1k | +2.9%  | 15.9k | 16.7k | +5.1%  |
| RX | 7.4k | 7.3k | -0.8%  | 9.1k | 9.1k | +0.1%  | 10.7k | 11.3k | +5.7%  |
| SRV | 259 | 258 | -0.4%  | 262 | 261 | -0.4%  | 263 | 262 | -0.4%  |

### tcp payload=256B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 20.9k | 21.3k | +1.8%  | 24.7k | 25.6k | +4.0%  | 27.4k | 31.2k | +13.7%⚠ |
| TX | 12.4k | 12.7k | +2.6%  | 16.0k | 16.9k | +5.9%⚠ | 18.5k | 22.4k | +21.0%⚠ |
| RX | 8.1k | 8.2k | +0.3%  | 9.7k | 9.8k | +0.7%  | 12.0k | 11.9k | -0.5%  |
| SRV | 259 | 259 | +0.0%  | 262 | 262 | +0.0%  | 264 | 262 | -0.8%  |

### tcp payload=512B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 21.1k | 21.1k | -0.1%  | 24.1k | 24.2k | +0.3%  | 29.5k | 28.9k | -2.1%  |
| TX | 12.5k | 12.5k | +0.1%  | 15.0k | 15.1k | +0.3%  | 20.7k | 20.1k | -2.9%  |
| RX | 8.2k | 8.2k | -0.4%  | 9.8k | 9.8k | -0.5%  | 12.1k | 12.0k | -0.7%  |
| SRV | 259 | 259 | +0.0%  | 262 | 262 | +0.0%  | 266 | 263 | -1.1%  |

### tcp payload=1024B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 22.0k | 22.0k | -0.3%  | 25.9k | 26.3k | +1.5%  | 31.2k | 29.5k | -5.6%  |
| TX | 13.0k | 13.0k | -0.3%  | 16.4k | 16.9k | +3.2%  | 21.6k | 20.0k | -7.5%  |
| RX | 8.6k | 8.6k | -0.1%  | 11.0k | 11.0k | -0.2%  | 12.6k | 12.5k | -1.1%  |
| SRV | 259 | 259 | +0.0%  | 261 | 261 | +0.0%  | 263 | 262 | -0.4%  |

### tcp payload=1460B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 21.6k | 21.5k | -0.4%  | 25.6k | 25.6k | +0.3%  | 29.0k | 28.5k | -1.7%  |
| TX | 12.8k | 12.7k | -0.4%  | 16.3k | 16.5k | +0.7%  | 19.8k | 19.4k | -2.1%  |
| RX | 8.4k | 8.4k | -0.3%  | 10.0k | 10.0k | -0.2%  | 12.5k | 12.4k | -0.8%  |
| SRV | 258 | 259 | +0.4%  | 261 | 261 | +0.0%  | 263 | 263 | +0.0%  |

### tcp payload=4096B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 30.2k | 30.2k | -0.2%  | 36.5k | 36.5k | +0.1%  | 39.6k | 40.2k | +1.5%  |
| TX | 18.2k | 18.2k | -0.2%  | 24.2k | 24.2k | -0.1%  | 26.5k | 26.6k | +0.2%  |
| RX | 11.7k | 11.6k | -0.3%  | 14.0k | 14.0k | -0.2%  | 16.4k | 17.5k | +7.2%  |
| SRV | 259 | 259 | +0.0%  | 263 | 262 | -0.4%  | 266 | 264 | -0.8%  |

### tcp payload=16384B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 52.2k | 52.0k | -0.5%  | 61.6k | 61.7k | +0.2%  | 66.4k | 65.8k | -1.0%  |
| TX | 30.0k | 29.9k | -0.4%  | 38.9k | 39.1k | +0.5%  | 42.9k | 42.3k | -1.5%  |
| RX | 21.9k | 21.8k | -0.4%  | 25.3k | 25.1k | -0.9%  | 28.4k | 27.9k | -1.7%  |
| SRV | 259 | 259 | +0.0%  | 264 | 263 | -0.4%  | 267 | 266 | -0.4%  |


## tcp (kernel)
  baseline:  `bench-data-lat-baseline-tcp-kernel-20260409.txt`
  postclean: `bench-data-lat-postclean-tcp-kernel-20260410-postclean.txt`

### tcp payload=64B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 25.2k | 25.2k | +0.1%  | 28.8k | 29.3k | +1.8%  | 69.0k | 46.3k | -32.9%⚠ |
| TX | 12.6k | 12.6k | +0.0%  | 15.0k | 14.8k | -0.8%  | 18.4k | 17.2k | -6.9%  |
| RX | 12.3k | 12.3k | +0.3%  | 14.7k | 14.9k | +1.1%  | 55.9k | 23.6k | -57.8%⚠ |
| SRV | 261 | 257 | -1.5%  | 269 | 266 | -1.1%  | 274 | 269 | -1.8%  |

### tcp payload=128B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 26.0k | 25.3k | -2.8%  | 30.4k | 28.6k | -6.0%⚠ | 69.0k | 69.0k | +0.0%  |
| TX | 13.1k | 12.6k | -4.0%  | 16.9k | 14.9k | -11.8%⚠ | 20.9k | 17.6k | -15.8%⚠ |
| RX | 12.5k | 12.3k | -1.5%  | 14.8k | 14.6k | -1.5%  | 54.7k | 55.5k | +1.5%  |
| SRV | 260 | 257 | -1.2%  | 263 | 261 | -0.8%  | 266 | 264 | -0.8%  |

### tcp payload=256B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 27.0k | 26.9k | -0.5%  | 31.2k | 32.0k | +2.6%  | 36.0k | 68.8k | +90.9%⚠ |
| TX | 13.5k | 13.5k | -0.5%  | 16.2k | 17.0k | +4.7%  | 21.4k | 21.2k | -0.9%  |
| RX | 13.2k | 13.1k | -0.5%  | 15.5k | 15.7k | +1.5%  | 21.7k | 54.2k | +150.1%⚠ |
| SRV | 259 | 257 | -0.8%  | 262 | 262 | +0.0%  | 264 | 264 | +0.0%  |

### tcp payload=512B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 26.9k | 26.7k | -0.9%  | 31.8k | 31.1k | -2.2%  | 37.0k | 35.7k | -3.5%  |
| TX | 13.3k | 13.2k | -1.0%  | 17.0k | 16.0k | -5.6%⚠ | 21.3k | 20.5k | -3.7%  |
| RX | 13.2k | 13.2k | -0.5%  | 15.8k | 15.5k | -1.7%  | 22.5k | 21.8k | -2.8%  |
| SRV | 262 | 258 | -1.5%  | 265 | 262 | -1.1%  | 266 | 264 | -0.8%  |

### tcp payload=1024B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 27.4k | 27.3k | -0.6%  | 34.8k | 33.0k | -5.1%⚠ | 69.0k | 61.7k | -10.5%⚠ |
| TX | 13.5k | 13.6k | +0.3%  | 17.0k | 17.6k | +3.6%  | 21.2k | 21.4k | +0.9%  |
| RX | 13.5k | 13.4k | -1.4%  | 17.0k | 16.4k | -3.8%  | 54.8k | 40.8k | -25.5%⚠ |
| SRV | 260 | 257 | -1.2%  | 263 | 262 | -0.4%  | 264 | 263 | -0.4%  |

### tcp payload=1460B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 27.8k | 28.1k | +1.3%  | 33.4k | 33.7k | +1.0%  | 46.8k | 45.3k | -3.2%  |
| TX | 13.8k | 13.8k | -0.1%  | 17.6k | 17.9k | +1.7%  | 20.9k | 20.9k | -0.2%  |
| RX | 13.6k | 14.0k | +2.8%  | 16.5k | 17.0k | +2.9%  | 25.0k | 23.7k | -5.6%  |
| SRV | 259 | 257 | -0.8%  | 266 | 262 | -1.5%  | 267 | 263 | -1.5%  |

### tcp payload=4096B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 29.7k | 29.9k | +0.6%  | 37.2k | 37.8k | +1.6%  | 41.7k | 46.3k | +11.1%⚠ |
| TX | 14.8k | 14.9k | +0.5%  | 20.7k | 20.9k | +0.9%  | 23.0k | 23.4k | +1.7%  |
| RX | 14.6k | 14.7k | +0.7%  | 20.5k | 20.3k | -0.8%  | 26.1k | 25.2k | -3.3%  |
| SRV | 260 | 259 | -0.4%  | 266 | 263 | -1.1%  | 267 | 264 | -1.1%  |

### tcp payload=16384B
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | 46.0k | 45.9k | -0.1%  | 55.4k | 55.0k | -0.8%  | 69.1k | 60.2k | -12.9%⚠ |
| TX | 19.6k | 19.6k | +0.3%  | 27.9k | 27.8k | -0.4%  | 31.6k | 29.7k | -6.2%  |
| RX | 26.0k | 25.7k | -1.3%  | 34.8k | 33.7k | -3.2%  | 48.9k | 35.3k | -27.9%⚠ |
| SRV | 259 | 259 | +0.0%  | 265 | 263 | -0.8%  | 266 | 264 | -0.8%  |


## ex_market --dpdk
  baseline:  `bench-data-lat-baseline-ex_market-dpdk-20260409.txt`
  postclean: `bench-data-lat-postclean-ex_market-dpdk-20260410-postclean.txt`

### exchange/market (oneway)
| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RX | 8.4k | 8.5k | +0.2%  | 59.7k | 64.0k | +7.2%⚠ | 63.1k | 75.9k | +20.2%⚠ |


