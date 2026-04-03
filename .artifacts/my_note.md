多币种，每次`rx_brust`，每个币种取最新。
1. p99 瓶颈是一次 `rx_brust`，最后一个WS Frame的延迟包含前面所有Frame的延迟。
2. 两阶段扫描优化，跳过了一些WS解析。
