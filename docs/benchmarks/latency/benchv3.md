使用新的hashmap

```bash
./bin/latency_matching
Pinning thread 123885950285696 to core 2.
=== MatchingEngine 专项 Benchmark (Cycles + ns 版) ===
  当前绑核: 2
  TSC 频率: 3.26744 GHz
  订单数: 100000  证券数: 10  簿深度: 5000  热身: 1000

=== Bench: addOrder ===
  [addOrder]  N=100000  tput=1911976 op/s
    mean:   1647.5 cycles  (   504.2 ns)
    p50:    1350.0 cycles  (   413.2 ns)
    p95:    2646.0 cycles  (   809.8 ns)
    p99:    5122.0 cycles  (  1567.6 ns)
    max:  463436.0 cycles  (141834.7 ns)

=== Bench: match (有成交) ===
  订单簿已填充 50000 笔
  [match(hit)]  N=100000  tput=8187494 op/s
    mean:    337.2 cycles  (   103.2 ns)
    p50:     302.0 cycles  (    92.4 ns)
    p95:     430.0 cycles  (   131.6 ns)
    p99:     866.0 cycles  (   265.0 ns)
    max:   63446.0 cycles  ( 19417.7 ns)
    成交笔数: 100000

=== Bench: match (无成交) ===
  订单簿已填充 50000 笔 (仅买单)
  [match(miss)]  N=100000  tput=13697351 op/s
    mean:    191.6 cycles  (    58.6 ns)
    p50:     184.0 cycles  (    56.3 ns)
    p95:     206.0 cycles  (    63.0 ns)
    p99:     228.0 cycles  (    69.8 ns)
    max:  156504.0 cycles  ( 47898.1 ns)

=== Bench: cancelOrder ===
  [cancelOrder]  N=100000  tput=2055197 op/s
    mean:   1537.4 cycles  (   470.5 ns)
    p50:    1438.0 cycles  (   440.1 ns)
    p95:    2242.0 cycles  (   686.2 ns)
    p99:    2766.0 cycles  (   846.5 ns)
    max:  470177.0 cycles  (143897.7 ns)

=== 完成 ===
```