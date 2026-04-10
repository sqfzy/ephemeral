# Latency benchmark baseline — 2026-04-09

Captured at commit `c88e1dc` on EC2 ARM64 (ens34=kernel, ens35=vfio-pci).
Each scenario was run via `sudo ./benchmarks/latency/lat <scenario> [--dpdk]`.
All values in **nanoseconds**. n = sample count.

## Files

| File | Size |
| --- | --- |
| `bench-data-lat-baseline-ex_market-dpdk-20260409.txt` | 2892 bytes |
| `bench-data-lat-baseline-ex_market-kernel-20260409.txt` | 1808 bytes |
| `bench-data-lat-baseline-ex_md_udp-dpdk-20260409.txt` | 4886 bytes |
| `bench-data-lat-baseline-ex_md_udp-kernel-20260409.txt` | 3782 bytes |
| `bench-data-lat-baseline-ex_order-dpdk-20260409.txt` | 5158 bytes |
| `bench-data-lat-baseline-ex_order-kernel-20260409.txt` | 4086 bytes |
| `bench-data-lat-baseline-tcp-dpdk-20260409.txt` | 13030 bytes |
| `bench-data-lat-baseline-tcp-kernel-20260409.txt` | 9495 bytes |
| `bench-data-lat-baseline-udp-dpdk-20260409.txt` | 6162 bytes |
| `bench-data-lat-baseline-udp-kernel-20260409.txt` | 4925 bytes |
| `bench-data-lat-baseline-ws-dpdk-20260409.txt` | 6279 bytes |
| `bench-data-lat-baseline-ws-kernel-20260409.txt` | 5120 bytes |

## `ex_market`

### exchange/market (oneway)

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RX | kernel | 39,061 | 10.8k | 21.0k | 194k | 267k | 643k |
| RX | dpdk | 27,765 | 7.0k | 8.4k | 59.7k | 63.1k | 66.6k |

## `ex_md_udp`

### exchange/md_udp payload=64B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 400,548 | 22.1k | 24.6k | 29.7k | 33.5k | 111k |
| RTT | dpdk | 528,716 | 17.2k | 18.7k | 21.6k | 26.5k | 114k |
| TX | kernel | 400,548 | 10.9k | 12.1k | 15.9k | 19.1k | 98.3k |
| TX | dpdk | 528,716 | 10.1k | 11.0k | 13.5k | 18.2k | 106k |
| RX | kernel | 400,548 | 10.5k | 12.0k | 15.5k | 18.6k | 94.1k |
| RX | dpdk | 528,716 | 6.5k | 7.3k | 8.9k | 11.0k | 35.0k |
| SRV | kernel | 400,548 | 242 | 260 | 264 | 1.0k | 7.5k |
| SRV | dpdk | 528,716 | 241 | 257 | 261 | 262 | 12.8k |

### exchange/md_udp payload=256B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 373,753 | 23.8k | 26.2k | 32.7k | 69.0k | 15301k |
| RTT | dpdk | 486,589 | 18.6k | 20.3k | 24.0k | 35.9k | 820k |
| TX | kernel | 373,753 | 11.7k | 13.0k | 16.9k | 28.6k | 407k |
| TX | dpdk | 486,589 | 10.3k | 11.9k | 15.0k | 26.3k | 809k |
| RX | kernel | 373,753 | 11.3k | 12.7k | 16.0k | 50.9k | 15282k |
| RX | dpdk | 486,589 | 7.2k | 8.0k | 9.6k | 12.4k | 61.1k |
| SRV | kernel | 373,753 | 246 | 260 | 264 | 1.0k | 7.1k |
| SRV | dpdk | 486,589 | 241 | 257 | 264 | 268 | 7.1k |

### exchange/md_udp payload=1024B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 365,515 | 24.0k | 26.9k | 37.3k | 84.8k | 3222k |
| RTT | dpdk | 475,782 | 19.0k | 20.8k | 24.8k | 40.2k | 404k |
| TX | kernel | 365,515 | 11.9k | 13.3k | 17.4k | 32.2k | 565k |
| TX | dpdk | 475,782 | 11.0k | 12.1k | 15.5k | 31.2k | 393k |
| RX | kernel | 365,515 | 11.5k | 13.0k | 16.3k | 53.3k | 3208k |
| RX | dpdk | 475,782 | 7.4k | 8.3k | 9.8k | 12.3k | 84.4k |
| SRV | kernel | 365,515 | 241 | 260 | 264 | 1.0k | 7.0k |
| SRV | dpdk | 475,782 | 242 | 257 | 266 | 269 | 12.9k |

### exchange/md_udp payload=1400B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 362,080 | 24.2k | 27.1k | 38.1k | 79.1k | 2659k |
| RTT | dpdk | 472,910 | 19.3k | 20.9k | 24.7k | 40.0k | 121k |
| TX | kernel | 362,080 | 12.0k | 13.5k | 17.6k | 32.8k | 2637k |
| TX | dpdk | 472,910 | 11.1k | 12.1k | 15.4k | 31.2k | 111k |
| RX | kernel | 362,080 | 11.6k | 13.1k | 16.7k | 51.3k | 551k |
| RX | dpdk | 472,910 | 7.5k | 8.3k | 9.9k | 12.2k | 46.8k |
| SRV | kernel | 362,080 | 246 | 260 | 264 | 1.0k | 13.7k |
| SRV | dpdk | 472,910 | 241 | 257 | 266 | 269 | 7.1k |

## `ex_order`

### exchange/order inflight=1

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 363,711 | 21.2k | 23.4k | 69.8k | 91.4k | 144k |
| RTT | dpdk | 546,678 | 15.8k | 17.2k | 57.9k | 72.2k | 147k |
| TX | kernel | 363,711 | 9.1k | 10.2k | 12.8k | 15.0k | 88.3k |
| TX | dpdk | 546,678 | 8.2k | 9.2k | 11.8k | 23.0k | 121k |
| RX | kernel | 363,711 | 10.4k | 12.9k | 58.9k | 79.3k | 133k |
| RX | dpdk | 546,678 | 6.8k | 7.6k | 46.4k | 60.8k | 131k |
| SRV | kernel | 363,711 | 283 | 302 | 321 | 330 | 8.9k |
| SRV | dpdk | 546,678 | 284 | 307 | 326 | 329 | 6.8k |

### exchange/order inflight=4

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 695,685 | 22.9k | 50.9k | 138k | 140k | 1926k |
| RTT | dpdk | 1,527,508 | 16.7k | 25.1k | 38.1k | 42.3k | 119k |
| TX | kernel | 695,685 | 9.2k | 23.2k | 68.9k | 74.1k | 326k |
| TX | dpdk | 1,527,508 | 8.5k | 11.6k | 17.2k | 19.9k | 102k |
| RX | kernel | 695,685 | 10.7k | 28.6k | 116k | 127k | 1912k |
| RX | dpdk | 1,527,508 | 7.1k | 13.0k | 23.4k | 26.3k | 107k |
| SRV | kernel | 695,685 | 281 | 300 | 317 | 325 | 12.1k |
| SRV | dpdk | 1,527,508 | 282 | 305 | 319 | 325 | 7.2k |

### exchange/order inflight=16

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 2,549,160 | 21.8k | 55.1k | 138k | 142k | 1091k |
| RTT | dpdk | 4,013,917 | 21.8k | 39.4k | 53.5k | 60.2k | 176k |
| TX | kernel | 2,549,160 | 9.2k | 22.6k | 70.4k | 76.7k | 240k |
| TX | dpdk | 4,013,917 | 9.3k | 22.7k | 35.2k | 40.5k | 156k |
| RX | kernel | 2,549,160 | 10.9k | 32.2k | 104k | 119k | 1042k |
| RX | dpdk | 4,013,917 | 7.3k | 16.0k | 25.9k | 30.4k | 122k |
| SRV | kernel | 2,549,160 | 283 | 300 | 308 | 319 | 15.9k |
| SRV | dpdk | 4,013,917 | 282 | 304 | 317 | 322 | 9.2k |

### exchange/order inflight=64

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 5,851,262 | 79.6k | 104k | 148k | 178k | 284k |
| RTT | dpdk | 6,046,889 | 62.2k | 106k | 131k | 145k | 235k |
| TX | kernel | 5,851,262 | 10.3k | 23.4k | 61.0k | 74.4k | 251k |
| TX | dpdk | 6,046,889 | 21.1k | 79.7k | 109k | 123k | 214k |
| RX | kernel | 5,851,262 | 17.5k | 80.2k | 111k | 133k | 184k |
| RX | dpdk | 6,046,889 | 7.9k | 25.0k | 40.3k | 47.0k | 128k |
| SRV | kernel | 5,851,262 | 282 | 300 | 308 | 315 | 12.1k |
| SRV | dpdk | 6,046,889 | 282 | 306 | 317 | 321 | 24.4k |

## `tcp`

### tcp payload=64B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 392,317 | 22.6k | 25.2k | 28.8k | 69.0k | 1474k |
| RTT | dpdk | 518,711 | 17.4k | 19.0k | 22.9k | 26.2k | 443k |
| TX | kernel | 392,317 | 11.0k | 12.6k | 15.0k | 18.4k | 1026k |
| TX | dpdk | 518,711 | 10.4k | 11.4k | 13.7k | 16.3k | 433k |
| RX | kernel | 392,317 | 11.0k | 12.3k | 14.7k | 55.9k | 1287k |
| RX | dpdk | 518,711 | 6.5k | 7.2k | 9.5k | 11.9k | 87.7k |
| SRV | kernel | 392,317 | 244 | 261 | 269 | 274 | 5.4k |
| SRV | dpdk | 518,711 | 242 | 259 | 269 | 270 | 6.2k |

### tcp payload=128B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 378,028 | 23.8k | 26.0k | 30.4k | 69.0k | 1713k |
| RTT | dpdk | 512,461 | 17.6k | 19.3k | 21.7k | 24.3k | 13730k |
| TX | kernel | 378,028 | 11.8k | 13.1k | 16.9k | 20.9k | 1677k |
| TX | dpdk | 512,461 | 10.4k | 11.5k | 13.7k | 15.9k | 13707k |
| RX | kernel | 378,028 | 11.4k | 12.5k | 14.8k | 54.7k | 953k |
| RX | dpdk | 512,461 | 6.6k | 7.4k | 9.1k | 10.7k | 37.0k |
| SRV | kernel | 378,028 | 244 | 260 | 263 | 266 | 6.3k |
| SRV | dpdk | 512,461 | 242 | 259 | 262 | 263 | 5.9k |

### tcp payload=256B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 366,457 | 24.9k | 27.0k | 31.2k | 36.0k | 92.8k |
| RTT | dpdk | 474,077 | 18.9k | 20.9k | 24.7k | 27.4k | 335k |
| TX | kernel | 366,457 | 12.3k | 13.5k | 16.2k | 21.4k | 76.8k |
| TX | dpdk | 474,077 | 11.1k | 12.4k | 16.0k | 18.5k | 325k |
| RX | kernel | 366,457 | 11.9k | 13.2k | 15.5k | 21.7k | 46.7k |
| RX | dpdk | 474,077 | 7.3k | 8.1k | 9.7k | 12.0k | 53.1k |
| SRV | kernel | 366,457 | 244 | 259 | 262 | 264 | 17.8k |
| SRV | dpdk | 474,077 | 243 | 259 | 262 | 264 | 14.5k |

### tcp payload=512B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 367,398 | 24.5k | 26.9k | 31.8k | 37.0k | 63.0k |
| RTT | dpdk | 469,054 | 19.2k | 21.1k | 24.1k | 29.5k | 10565k |
| TX | kernel | 367,398 | 11.8k | 13.3k | 17.0k | 21.3k | 43.0k |
| TX | dpdk | 469,054 | 11.3k | 12.5k | 15.0k | 20.7k | 10541k |
| RX | kernel | 367,398 | 12.0k | 13.2k | 15.8k | 22.5k | 42.8k |
| RX | dpdk | 469,054 | 7.3k | 8.2k | 9.8k | 12.1k | 61.1k |
| SRV | kernel | 367,398 | 243 | 262 | 265 | 266 | 6.0k |
| SRV | dpdk | 469,054 | 242 | 259 | 262 | 266 | 6.6k |

### tcp payload=1024B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 360,012 | 25.2k | 27.4k | 34.8k | 69.0k | 1577k |
| RTT | dpdk | 448,283 | 19.5k | 22.0k | 25.9k | 31.2k | 100.0k |
| TX | kernel | 360,012 | 12.2k | 13.5k | 17.0k | 21.2k | 1547k |
| TX | dpdk | 448,283 | 11.5k | 13.0k | 16.4k | 21.6k | 91.3k |
| RX | kernel | 360,012 | 12.3k | 13.5k | 17.0k | 54.8k | 582k |
| RX | dpdk | 448,283 | 7.4k | 8.6k | 11.0k | 12.6k | 57.3k |
| SRV | kernel | 360,012 | 244 | 260 | 263 | 264 | 7.7k |
| SRV | dpdk | 448,283 | 251 | 259 | 261 | 263 | 7.6k |

### tcp payload=1460B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 356,175 | 25.5k | 27.8k | 33.4k | 46.8k | 1525k |
| RTT | dpdk | 459,225 | 19.6k | 21.6k | 25.6k | 29.0k | 468k |
| TX | kernel | 356,175 | 12.5k | 13.8k | 17.6k | 20.9k | 1328k |
| TX | dpdk | 459,225 | 11.5k | 12.8k | 16.3k | 19.8k | 456k |
| RX | kernel | 356,175 | 12.2k | 13.6k | 16.5k | 25.0k | 1510k |
| RX | dpdk | 459,225 | 7.5k | 8.4k | 10.0k | 12.5k | 65.9k |
| SRV | kernel | 356,175 | 242 | 259 | 266 | 267 | 7.5k |
| SRV | dpdk | 459,225 | 241 | 258 | 261 | 263 | 7.7k |

### tcp payload=4096B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 332,521 | 27.1k | 29.7k | 37.2k | 41.7k | 115k |
| RTT | dpdk | 327,542 | 25.6k | 30.2k | 36.5k | 39.6k | 961k |
| TX | kernel | 332,521 | 13.2k | 14.8k | 20.7k | 23.0k | 99.2k |
| TX | dpdk | 327,542 | 14.7k | 18.2k | 24.2k | 26.5k | 946k |
| RX | kernel | 332,521 | 13.1k | 14.6k | 20.5k | 26.1k | 47.6k |
| RX | dpdk | 327,542 | 10.0k | 11.7k | 14.0k | 16.4k | 60.6k |
| SRV | kernel | 332,521 | 242 | 260 | 266 | 267 | 8.7k |
| SRV | dpdk | 327,542 | 243 | 259 | 263 | 266 | 7.3k |

### tcp payload=16384B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 216,314 | 35.7k | 46.0k | 55.4k | 69.1k | 1969k |
| RTT | dpdk | 189,859 | 45.4k | 52.2k | 61.6k | 66.4k | 488k |
| TX | kernel | 216,314 | 17.5k | 19.6k | 27.9k | 31.6k | 407k |
| TX | dpdk | 189,859 | 24.9k | 30.0k | 38.9k | 42.9k | 462k |
| RX | kernel | 216,314 | 16.8k | 26.0k | 34.8k | 48.9k | 1945k |
| RX | dpdk | 189,859 | 18.7k | 21.9k | 25.3k | 28.4k | 93.5k |
| SRV | kernel | 216,314 | 244 | 259 | 265 | 266 | 10.4k |
| SRV | dpdk | 189,859 | 243 | 259 | 264 | 267 | 8.8k |

## `udp`

### udp payload=64B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 411,028 | 21.7k | 23.9k | 28.3k | 32.3k | 112k |
| RTT | dpdk | 526,931 | 17.0k | 18.7k | 23.0k | 26.2k | 111k |
| TX | kernel | 411,028 | 10.9k | 12.0k | 15.4k | 18.3k | 92.0k |
| TX | dpdk | 526,931 | 10.1k | 11.0k | 13.7k | 17.8k | 104k |
| RX | kernel | 411,028 | 10.2k | 11.5k | 14.8k | 17.5k | 99.9k |
| RX | dpdk | 526,931 | 6.5k | 7.3k | 9.6k | 11.6k | 36.5k |
| SRV | kernel | 411,028 | 244 | 262 | 270 | 274 | 6.8k |
| SRV | dpdk | 526,931 | 243 | 260 | 264 | 273 | 8.1k |

### udp payload=128B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 408,823 | 21.7k | 24.0k | 28.8k | 41.3k | 1190k |
| RTT | dpdk | 524,876 | 17.3k | 18.8k | 23.2k | 26.6k | 145k |
| TX | kernel | 408,823 | 10.9k | 12.0k | 15.7k | 19.3k | 150k |
| TX | dpdk | 524,876 | 10.0k | 11.0k | 13.7k | 18.6k | 136k |
| RX | kernel | 408,823 | 10.2k | 11.5k | 15.0k | 19.5k | 1175k |
| RX | dpdk | 524,876 | 6.5k | 7.3k | 9.6k | 11.5k | 39.2k |
| SRV | kernel | 408,823 | 247 | 263 | 270 | 273 | 12.9k |
| SRV | dpdk | 524,876 | 245 | 260 | 270 | 275 | 11.8k |

### udp payload=256B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 385,690 | 22.9k | 25.4k | 32.0k | 69.0k | 1616k |
| RTT | dpdk | 488,295 | 18.5k | 20.2k | 24.6k | 36.7k | 1118k |
| TX | kernel | 385,690 | 11.5k | 12.8k | 16.8k | 26.2k | 1134k |
| TX | dpdk | 488,295 | 10.8k | 11.8k | 15.1k | 25.6k | 1096k |
| RX | kernel | 385,690 | 10.9k | 12.2k | 15.3k | 48.2k | 1602k |
| RX | dpdk | 488,295 | 7.1k | 8.0k | 9.9k | 12.3k | 55.5k |
| SRV | kernel | 385,690 | 244 | 262 | 271 | 274 | 18.3k |
| SRV | dpdk | 488,295 | 244 | 260 | 270 | 273 | 12.6k |

### udp payload=512B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 382,384 | 23.6k | 25.6k | 31.4k | 45.7k | 132k |
| RTT | dpdk | 484,636 | 18.6k | 20.4k | 24.5k | 37.0k | 369k |
| TX | kernel | 382,384 | 11.6k | 12.9k | 16.7k | 26.5k | 102k |
| TX | dpdk | 484,636 | 10.8k | 11.9k | 14.9k | 25.8k | 352k |
| RX | kernel | 382,384 | 11.1k | 12.4k | 15.4k | 29.6k | 118k |
| RX | dpdk | 484,636 | 7.3k | 8.1k | 9.9k | 12.1k | 39.4k |
| SRV | kernel | 382,384 | 243 | 262 | 266 | 273 | 7.4k |
| SRV | dpdk | 484,636 | 244 | 260 | 270 | 272 | 7.3k |

### udp payload=1024B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 374,503 | 23.7k | 26.1k | 34.5k | 71.2k | 123k |
| RTT | dpdk | 478,053 | 18.8k | 20.6k | 25.0k | 42.1k | 117k |
| TX | kernel | 374,503 | 11.7k | 13.1k | 17.3k | 30.2k | 106k |
| TX | dpdk | 478,053 | 11.0k | 12.0k | 15.3k | 31.3k | 108k |
| RX | kernel | 374,503 | 11.2k | 12.6k | 15.8k | 32.5k | 109k |
| RX | dpdk | 478,053 | 7.4k | 8.2k | 10.1k | 12.4k | 36.7k |
| SRV | kernel | 374,503 | 247 | 262 | 271 | 275 | 8.8k |
| SRV | dpdk | 478,053 | 246 | 260 | 270 | 272 | 13.9k |

### udp payload=1472B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 370,769 | 23.7k | 26.4k | 36.4k | 70.2k | 1367k |
| RTT | dpdk | 472,941 | 19.0k | 20.8k | 25.3k | 43.6k | 937k |
| TX | kernel | 370,769 | 11.9k | 13.2k | 17.5k | 30.8k | 1343k |
| TX | dpdk | 472,941 | 11.1k | 12.1k | 15.5k | 32.6k | 924k |
| RX | kernel | 370,769 | 11.3k | 12.8k | 16.1k | 54.0k | 859k |
| RX | dpdk | 472,941 | 7.5k | 8.4k | 10.3k | 12.6k | 65.7k |
| SRV | kernel | 370,769 | 247 | 262 | 270 | 274 | 9.7k |
| SRV | dpdk | 472,941 | 244 | 260 | 269 | 271 | 7.8k |

## `ws`

### ws payload=64B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 377,167 | 24.1k | 26.1k | 29.4k | 69.0k | 1011k |
| RTT | dpdk | 500,799 | 18.2k | 19.7k | 22.2k | 25.0k | 53.8k |
| TX | kernel | 377,167 | 11.6k | 12.7k | 15.0k | 17.8k | 224k |
| TX | dpdk | 500,799 | 10.9k | 11.8k | 14.0k | 16.3k | 45.1k |
| RX | kernel | 377,167 | 11.8k | 13.0k | 15.4k | 55.6k | 998k |
| RX | dpdk | 500,799 | 6.7k | 7.5k | 9.3k | 10.9k | 42.1k |
| SRV | kernel | 377,167 | 264 | 286 | 303 | 310 | 7.0k |
| SRV | dpdk | 500,799 | 281 | 298 | 305 | 307 | 6.5k |

### ws payload=128B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 374,846 | 24.4k | 26.4k | 29.3k | 32.1k | 87.7k |
| RTT | dpdk | 497,918 | 18.1k | 19.8k | 22.4k | 25.3k | 1125k |
| TX | kernel | 374,846 | 11.8k | 12.9k | 15.2k | 17.5k | 47.8k |
| TX | dpdk | 497,918 | 11.0k | 11.9k | 14.2k | 16.6k | 1110k |
| RX | kernel | 374,846 | 11.9k | 13.0k | 15.3k | 17.8k | 72.0k |
| RX | dpdk | 497,918 | 6.7k | 7.5k | 9.3k | 11.0k | 84.9k |
| SRV | kernel | 374,846 | 266 | 287 | 303 | 305 | 6.3k |
| SRV | dpdk | 497,918 | 277 | 291 | 304 | 306 | 7.7k |

### ws payload=256B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 363,867 | 25.2k | 27.2k | 30.8k | 35.3k | 61.6k |
| RTT | dpdk | 472,019 | 19.2k | 20.9k | 24.2k | 27.9k | 521k |
| TX | kernel | 363,867 | 12.6k | 13.8k | 16.3k | 21.6k | 48.3k |
| TX | dpdk | 472,019 | 11.9k | 12.9k | 15.6k | 19.5k | 505k |
| RX | kernel | 363,867 | 11.9k | 13.0k | 15.2k | 17.8k | 43.2k |
| RX | dpdk | 472,019 | 6.7k | 7.6k | 9.6k | 11.5k | 63.6k |
| SRV | kernel | 363,867 | 266 | 282 | 298 | 301 | 7.4k |
| SRV | dpdk | 472,019 | 274 | 291 | 303 | 306 | 7.0k |

### ws payload=512B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 358,875 | 25.4k | 27.5k | 31.7k | 37.7k | 2710k |
| RTT | dpdk | 464,540 | 19.5k | 21.3k | 24.7k | 28.0k | 51.6k |
| TX | kernel | 358,875 | 12.9k | 14.1k | 16.9k | 22.1k | 375k |
| TX | dpdk | 464,540 | 12.2k | 13.2k | 16.1k | 19.7k | 43.5k |
| RX | kernel | 358,875 | 11.9k | 13.0k | 15.3k | 18.9k | 2695k |
| RX | dpdk | 464,540 | 6.7k | 7.6k | 9.6k | 11.5k | 38.3k |
| SRV | kernel | 358,875 | 269 | 293 | 303 | 358 | 12.2k |
| SRV | dpdk | 464,540 | 275 | 300 | 306 | 308 | 8.0k |

### ws payload=1024B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 347,337 | 26.1k | 28.2k | 35.8k | 69.2k | 2517k |
| RTT | dpdk | 450,003 | 20.3k | 21.9k | 26.0k | 29.0k | 109k |
| TX | kernel | 347,337 | 13.5k | 14.8k | 18.2k | 33.7k | 2473k |
| TX | dpdk | 450,003 | 12.9k | 14.0k | 17.7k | 20.7k | 98.9k |
| RX | kernel | 347,337 | 11.7k | 13.1k | 16.1k | 54.1k | 405k |
| RX | dpdk | 450,003 | 6.8k | 7.6k | 9.5k | 11.2k | 33.2k |
| SRV | kernel | 347,337 | 267 | 294 | 304 | 364 | 9.4k |
| SRV | dpdk | 450,003 | 271 | 300 | 306 | 308 | 8.1k |

### ws payload=4096B

| metric | mode | n | min | p50 | p99 | p999 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTT | kernel | 267,954 | 30.7k | 32.9k | 69.2k | 99.7k | 9810k |
| RTT | dpdk | 321,847 | 26.1k | 30.8k | 36.7k | 40.6k | 326k |
| TX | kernel | 267,954 | 17.6k | 19.1k | 25.6k | 43.6k | 9762k |
| TX | dpdk | 321,847 | 18.8k | 22.6k | 28.2k | 30.7k | 317k |
| RX | kernel | 267,954 | 12.2k | 13.5k | 50.0k | 54.3k | 673k |
| RX | dpdk | 321,847 | 6.6k | 7.8k | 9.8k | 13.4k | 42.7k |
| SRV | kernel | 267,954 | 272 | 295 | 303 | 370 | 7.5k |
| SRV | dpdk | 321,847 | 278 | 294 | 306 | 418 | 7.5k |

