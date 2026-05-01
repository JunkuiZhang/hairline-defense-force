```bash
./bin/latency_e2e
=== E2E Ping-Pong Latency Benchmark ===
TSC Frequency: 2.9952 GHz
初始化系统 (1 Bucket)...
Pinning thread 126233833265024 to core 2.
预挂 102000 笔买单入簿...
Pinning thread 126233724319424 to core 3.
预热 2000 + 测量 100000 笔...

=== 测试结果 (N=100000) ===
  Mean:   2898.1 cycles (   967.6 ns)
  p50 :     2414 cycles (   806.0 ns)
  p90 :     3098 cycles (  1034.3 ns)
  p95 :     3462 cycles (  1155.9 ns)
  p99 :    12980 cycles (  4333.7 ns)
  Max :   339760 cycles (113436.3 ns)
```