# Jungle Mini DB

C로 구현한 작은 SQL 처리기다. `users`, `posts` 테이블을 고정 길이 row 파일로 저장하고, `id` 조회는 B+Tree 인덱스를 통해 row 위치를 찾아 읽는다.

지원 SQL 예시:

```sql
select * from users;
select * from users where id = 101;
select * from users where id between 101 and 103;
insert into users values (104,new-user);
```

## Build

```bash
cmake -S . -B cmake-build-debug -G Ninja
cmake --build cmake-build-debug
```

## Run

```bash
./cmake-build-debug/jungle_mini_db
```

종료:

```text
.exit
```

## Dev Container

VS Code에서 팀 전체 개발 환경을 Linux 컨테이너로 통일할 수 있도록 `.devcontainer/` 설정을 포함한다.

사전 준비:

- Docker Desktop 또는 Docker Engine
- VS Code
- Dev Containers 확장

사용 방법:

```text
VS Code Command Palette -> Dev Containers: Reopen in Container
```

컨테이너가 처음 열리면 다음 도구가 자동으로 준비된다.

- Ubuntu 24.04
- build-essential
- CMake
- Ninja
- clang / clangd / clang-format
- gdb / valgrind
- Python 3

컨테이너 내부 기본 빌드 명령:

```bash
cmake -S . -B cmake-build-debug -G Ninja
cmake --build cmake-build-debug
./cmake-build-debug/jungle_mini_db
```

## Benchmark

`docs/004-bplus-tree-index.md`의 대규모 데이터 성능 테스트 조건을 확인하는 스크립트가 있다.

- 기본값으로 1,000,000개 레코드를 `INSERT` SQL로 적재한다.
- `id` 기준 SELECT는 미니 DB의 B+Tree 인덱스 경로를 사용한다.
- `name` 기준 SELECT는 fixed-row 데이터 파일을 선형 탐색한다.
- 두 SELECT 방식의 실행 시간을 비교한다.

스크립트는 Python 3 표준 라이브러리만 사용한다. venv나 패키지 설치가 필요 없다.

```bash
./scripts/benchmark_bplus_tree_index.py
```

결과를 JSON으로 저장:

```bash
./scripts/benchmark_bplus_tree_index.py --output benchmark-results/bplus-tree-index.json
```

빠른 동작 확인:

```bash
./scripts/benchmark_bplus_tree_index.py --records 1000 --select-repetitions 5 --load-mode generate --skip-build
```

기본적으로 벤치마크 실행 전 기존 `data/users.csv`, `data/users.idx`, `data/users.idx.boot`를 백업하고 실행 후 복원한다. 생성된 인덱스 파일과 `benchmark-results/`는 git에서 제외된다.
