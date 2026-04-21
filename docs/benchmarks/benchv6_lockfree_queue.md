# 性能记录 v6 (MPSC Lock-Free Queue)

## 优化背景

在之前的版本中，系统并发提交订单到 `WorkerBucket` 依赖于 `std::mutex` 和 `std::condition_variable` 保护的 `std::deque`。尽管已经通过多 Bucket 分摊了单锁的压力，但在极端的高频交易场景下，互斥锁依旧存在以下问题：
1. 这个锁涉及内核级的线程上下文切换（Context Switch）。
2. 在生产者之间以及生产者与消费者之间容易遭遇锁争用（Lock Contention）。
3. 长尾延迟（p99/max Latency）较高，导致撮合性能不够平滑和确定。

## 方案内容

为了追求极致的延迟表现和平滑吞吐，我们手写移除了所有 Mutex，代之以经典的无锁队列（Lock-Free Queue）：
1. **Dmitry Vyukov Bounded MPSC/MPMC Queue**：基于预分配内存的环形数组（Ring Buffer）。
2. **基于 Sequence 的锁消除**：每一个坑位（Slot）带有原子 `sequence`。通过原子的 `compare_exchange_weak` (CAS) 分配索引位置，并利用 `sequence` 等待该坑位的消费端读写状态。
3. **消费者忙轮询 (Busy Waiting/Yield)**：在 Worker 线程检测到队列为空时直接使用 `pause` 或 `yield` 让出 CPU 切片，而不是使用条件变量挂起当前线程。
4. **Cache Line Padding**：核心变量采用 `alignas(64)` 以避免伪共享（False Sharing）。

## 测试结果

执行命令：
```bash
./bin/bench_multicore --orders 1000000 --producers 8 --buckets 4
```

结果如下：
```text
=== 多 Bucket 并行扩展性 Benchmark ===
  总订单: 1000000  生产者: 8  证券数: 50  股东数: 500  热身: 2000
  硬件线程数: 12  测试 bucket 数: 4

>>> 4 bucket(s), 8 producers...--- Performance Metrics ---
Total Orders : 1000000
Cache Misses : 2122 (Avg 0.002122 per order)
Page Faults  : 10

──────────────────────────────────────────────────
  Buckets: 4   Producers: 8   Threads: 12
  订单数: 1000000   耗时: 1.013s   成交: 590840   拒绝: 444106
  吞吐量: 986759 orders/s
  E2E 延时(us):  p50=105950.55  p95=240763.75  p99=256956.64  max=260777.49
  入队延时(us):  p50=0.53  p95=34.45  p99=80.07  max=10757.06
```

## 结论分析

- **尾部延迟大幅下降**: 对比有锁版本（v5 中 p99 为 ~396,000 us），v6 将 `E2E p99` 降到了 ~256,000 us，**改善了 35% 以上的极端长尾延迟**，系统的实时性和平滑交易能力得到质的提升。
- **免内核切换开销**: 由于去除了所有的系统层面同步原语，程序在不同调度环境下表现得更加高度确定一致，完全运行在用户态。
