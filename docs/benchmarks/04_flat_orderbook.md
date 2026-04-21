=== MatchingEngine 专项 Benchmark ===
  订单数: 100000  证券数: 10  簿深度: 5000  热身: 1000

=== Bench: addOrder ===
  [addOrder]  N=100000  tput=2843251.54 op/s
    mean=0.33 us  p50=0.17  p95=0.40  p99=1.20  max=3582.01 us

=== Bench: match (有成交) ===
  订单簿已填充 50000 笔
  [match(hit)]  N=100000  tput=5278994.88 op/s
    mean=0.16 us  p50=0.13  p95=0.27  p99=0.56  max=20.90 us
    成交笔数: 100000

=== Bench: match (无成交) ===
  订单簿已填充 50000 笔 (仅买单)
  [match(miss)]  N=100000  tput=12822156.69 op/s
    mean=0.06 us  p50=0.05  p95=0.08  p99=0.11  max=132.20 us

=== Bench: cancelOrder ===
  [cancelOrder]  N=100000  tput=764718.93 op/s
    mean=1.28 us  p50=0.98  p95=3.09  p99=4.43  max=227.97 us

=== 完成 ===
