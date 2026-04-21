=== MatchingEngine 专项 Benchmark ===
  订单数: 100000  证券数: 10  簿深度: 5000  热身: 1000

=== Bench: addOrder ===
  [addOrder]  N=100000  tput=2615336.33 op/s
    mean=0.36 us  p50=0.20  p95=0.52  p99=1.31  max=3036.30 us

=== Bench: match (有成交) ===
  订单簿已填充 50000 笔
  [match(hit)]  N=100000  tput=5406574.39 op/s
    mean=0.16 us  p50=0.14  p95=0.19  p99=0.57  max=52.23 us
    成交笔数: 100000

=== Bench: match (无成交) ===
  订单簿已填充 50000 笔 (仅买单)
  [match(miss)]  N=100000  tput=12828736.37 op/s
    mean=0.06 us  p50=0.05  p95=0.07  p99=0.09  max=12.81 us

=== Bench: cancelOrder ===
  [cancelOrder]  N=100000  tput=605381.84 op/s
    mean=1.62 us  p50=1.18  p95=4.18  p99=6.78  max=136.19 us

=== 完成 ===
