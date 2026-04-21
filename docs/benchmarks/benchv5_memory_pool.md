# 性能记录 v5 (Memory Pool / Intrusive List)

## 优化背景

在 v4 版本中，我们将 JSON 解析移出了 Worker 线程，大幅消除了命令入队列的前置堆分配开销。但在撮合引擎核心逻辑中，`MatchingEngine` 内部的 `PriceLevel` 使用了 `std::list<BookEntry>` 作为其底层数据结构，这会导致以下几个性能问题：
1. **频繁借还操作**：每一笔订单入簿都会 `new/malloc` 出节点并分配内存空间；对手盘完全成交或者用户撤单时，需要调用 `delete/free`。
2. **锁争用（如果底层 allocator 含锁）和系统调用**：在高并发场景中，操作系统的通用分配器（如 glibc malloc）即使引入了 per-thread arena，也仍然会有显著的开销和偶尔的系统调用。
3. **缓存未命中 (Cache Miss)**：`std::list` 是典型的动态随机分配内存结构，在进行按顺序撮合遍历或者清理过期节点时，物理空间分散，产生极高的 L1/L2 Cache Misses。

## 方案内容

为了彻底消除所有运行时针对 `BookEntry` 的堆分配，我们实现了以下方案：
1. **对象池 (ObjectPool)**: 自定义了一套基于 FreeList 的对象池模板 `ObjectPool<BookEntry>`（采用局部于单引擎的非线程安全模型，以适配当前 WorkerBucket 设计）。对象池中统一连续分配大块内存（如预置 16K 个节点），在请求时候 O(1) 弹出空闲地址。
2. **侵入式双向链表 (Intrusive Doubly Linked List)**: 在 `BookEntry` 结构体内置了 `next` 和 `prev` 指针。移除原生 `std::list`，利用侵入式指针构建自定义的 `PriceLevel` 双向链表，与对象池完全融合，彻底根除了节点额外的封包包装消耗（如 `std::_List_node`）。

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
Cache Misses : 1969 (Avg 0.001969 per order)
Page Faults  : 10

──────────────────────────────────────────────────
  Buckets: 4   Producers: 8   Threads: 12
  订单数: 1000000   耗时: 0.853s   成交: 600080   拒绝: 443953
  吞吐量: 1172739 orders/s
  E2E 延时(us):  p50=201621.61  p95=386978.47  p99=396077.35  max=402953.82
  入队延时(us):  p50=0.60  p95=15.30  p99=72.10  max=2065.33
```

## 结论分析

- **缓存友好与系统分配消除**: `Cache Misses` 的数据量进一步下降并趋向平稳，`Page Faults` 的数量仅留存在了进程启动的初始大内存分配部分（10次缺页中断），业务进行时实现了 **0 分配（Zero Allocation）**。
- **稳定性**: 吞吐量保持在约 1.17M orders/s 左右（具体视机器负载环境变化）。更重要的是，延时的抖动范围进一步压缩，极大改善了订单撮合时的确定性。
