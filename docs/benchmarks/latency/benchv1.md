## Baseline

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target latency_matching -j $(nproc)
./bin/latency_matching
```

```bash
Pinning thread 136920875759488 to core 2.
=== MatchingEngine 专项 Benchmark (Cycles + ns 版) ===
  当前绑核: 2
  TSC 频率: 3.32797 GHz
  订单数: 100000  证券数: 10  簿深度: 5000  热身: 1000

=== Bench: addOrder ===
  [addOrder]  N=100000  tput=1587524 op/s
    mean:   2037.4 cycles  (   612.2 ns)
    p50:    1372.0 cycles  (   412.3 ns)
    p95:    3992.0 cycles  (  1199.5 ns)
    p99:    6242.0 cycles  (  1875.6 ns)
    max:  17162998.0 cycles  (5157202.7 ns)

=== Bench: match (有成交) ===
  订单簿已填充 50000 笔
  [match(hit)]  N=100000  tput=6439641 op/s
    mean:    443.4 cycles  (   133.2 ns)
    p50:     396.0 cycles  (   119.0 ns)
    p95:     604.0 cycles  (   181.5 ns)
    p99:    1298.0 cycles  (   390.0 ns)
    max:  386854.0 cycles  (116243.4 ns)
    成交笔数: 100000

=== Bench: match (无成交) ===
  订单簿已填充 50000 笔 (仅买单)
  [match(miss)]  N=100000  tput=18342468 op/s
    mean:    127.4 cycles  (    38.3 ns)
    p50:     120.0 cycles  (    36.1 ns)
    p95:     164.0 cycles  (    49.3 ns)
    p99:     310.0 cycles  (    93.1 ns)
    max:   27648.0 cycles  (  8307.8 ns)

=== Bench: cancelOrder ===
  [cancelOrder]  N=100000  tput=1543071 op/s
    mean:   2103.5 cycles  (   632.1 ns)
    p50:    1972.0 cycles  (   592.6 ns)
    p95:    3248.0 cycles  (   976.0 ns)
    p99:    4108.0 cycles  (  1234.4 ns)
    max:  359580.0 cycles  (108048.0 ns)

=== 完成 ===
```


```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target latency_multicore -j $(nproc)
./bin/latency_multicore
```

```bash
╔═════════════════════════════════════════════════════════════════════════════════╗
║  多 Bucket 并行扩展性测试 - 绑核 (Pinned)                           ║
╠═════════╦═══════════╦══════════════╦══════════════╦══════════════╦══════════════╣
║ Buckets ║ Throughput║ E2E-p50 (ns) ║ E2E-p99 (ns) ║ Enq-p50 (ns) ║   加速比     ║
╠═════════╬═══════════╬══════════════╬══════════════╬══════════════╬══════════════╣
║ 1       ║ 453527    ║ 196246708.7  ║ 359590514.4  ║ 247.0        ║ 1.00       x ║
║ 2       ║ 829122    ║ 88937821.4   ║ 153539872.3  ║ 352.8        ║ 1.83       x ║
║ 4       ║ 1310636   ║ 32969848.5   ║ 72453219.0   ║ 286.7        ║ 2.89       x ║
╚═════════╩═══════════╩══════════════╩══════════════╩══════════════╩══════════════╝
```
