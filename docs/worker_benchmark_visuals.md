# Worker Benchmark Visual Assets

발표용으로 바로 사용할 수 있도록 Markdown 표를 SVG 그래프로 변환한 결과다.

## 4.1 Read-heavy

조건: `read_heavy @ 3000 rps, 10s`

![4.1 Read-heavy](assets/worker-benchmark/01_read_heavy_baseline.svg)

## 4.2 Mixed

조건: `mixed @ 3000 rps, 20s`

![4.2 Mixed](assets/worker-benchmark/02_mixed_baseline.svg)

## 8.3 pilot 결과

`read_heavy @ 3000 rps, 10s, worker=1` 에서 먼저 pilot을 돌렸다.

![8.3 pilot 결과](assets/worker-benchmark/03_simulated_work_pilot.svg)

## 8.4 본 실험 결과

조건: `read_heavy @ 3000 rps, 10s, simulated_work_us=500`

![8.4 본 실험 결과](assets/worker-benchmark/04_read_heavy_simulated_work_500us.svg)
