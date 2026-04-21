=== MatchingEngine 专项 Benchmark ===
  订单数: 100000  证券数: 10  簿深度: 5000  热身: 1000

=== Bench: addOrder ===
  [addOrder]  N=100000  tput=2148273.86 op/s
    mean=0.44 us  p50=0.23  p95=0.70  p99=1.58  max=4380.32 us

=== Bench: match (有成交) ===
  订单簿已填充 50000 笔
  [match(hit)]  N=100000  tput=6470397.93 op/s
    mean=0.13 us  p50=0.11  p95=0.18  p99=0.54  max=33.69 us
    成交笔数: 100000

=== Bench: match (无成交) ===
  订单簿已填充 50000 笔 (仅买单)
  [match(miss)]  N=100000  tput=12416190.71 op/s
    mean=0.06 us  p50=0.06  p95=0.07  p99=0.08  max=11.56 us

=== Bench: cancelOrder ===
  [cancelOrder]  N=100000  tput=627967.14 op/s
    mean=1.57 us  p50=1.12  p95=4.12  p99=5.95  max=254.13 us

=== 完成 ===
