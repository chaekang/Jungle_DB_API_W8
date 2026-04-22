# Queue Size Presentation Visuals

발표에서 바로 사용할 핵심 메시지만 남긴 요약 시각화다.

## 발표 포인트

1. 작은 큐는 burst를 흡수하지 못하고 조기 503을 많이 낸다.
2. 큰 큐는 실패를 줄이는 대신 tail latency와 queue wait를 늘린다.
3. 현재 조건에서는 `48` 이 `32` 와 `64` 사이의 가장 설득력 있는 절충점이다.

## 이미지

### 포인트 1. 작은 큐는 burst를 흡수하지 못하고 너무 빨리 503을 낸다

![포인트 1. 작은 큐는 burst를 흡수하지 못하고 너무 빨리 503을 낸다](docs/assets/queue_size_presentation/01_small_queue_burst_failure.svg)

### 포인트 2. 큰 큐는 실패를 줄이지만 tail latency를 늘린다

![포인트 2. 큰 큐는 실패를 줄이지만 tail latency를 늘린다](docs/assets/queue_size_presentation/02_large_queue_tail_latency.svg)

### 포인트 3. read-heavy 에서는 48 이 32 와 64 사이의 절충점이다

![포인트 3. read-heavy 에서는 48 이 32 와 64 사이의 절충점이다](docs/assets/queue_size_presentation/03_final_candidate_read_heavy.svg)

### 포인트 4. mixed 에서도 48 이 32 와 64 사이의 가장 설득력 있는 절충점이다

![포인트 4. mixed 에서도 48 이 32 와 64 사이의 가장 설득력 있는 절충점이다](docs/assets/queue_size_presentation/04_final_candidate_mixed.svg)

