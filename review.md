• Findings

  1. 가장 큰 전제부터 문서와 현재 엔진이 어긋납니다. 현재 실행 경로는 main -> parser
     -> executor이고 src/main.c:57, 이 executor는 파일 저장소가 아니라 메모리 런타임
     만 사용합니다. src/executor.c:400 반면 docs/SPEC.md:3는 파일 기반 CSV, 메모리
     인덱스, flock(), DELETE 지원을 설명합니다. 지금 ## 4, ## 5는 이 둘 중 어느 엔진
     을 서버에 붙일지 먼저 고정하지 않으면 설계가 흔들립니다.
        TODO. 이거는 그럼 현재 프로젝트에 맞도록 너가 알아서 문서를 수정해줘.  현재 문서의 전제는 DB를 바꾸기 전을 기준으로 만들어졌어.
  2. 현재 엔진은 “테이블 단위/인덱스 단위 락”을 논하기 전에 전역 단일 런타임 문제를
     먼저 해결해야 합니다. 런타임 테이블은 static TableRuntime table_runtime_active;
     하나뿐이고 src/table_runtime.c:11, table_get_or_load()는 다른 테이블 이름이 오
     면 기존 런타임을 통째로 table_free()하고 새 테이블로 바꿉니다. src/
     table_runtime.c:239 테스트도 “테이블 전환 시 rows가 초기화된다”를 기대합니다.
     tests/test_table_runtime.c:117 이 상태에서는 다른 테이블끼리 병렬 처리 자체가
     불가능하고, 엄밀히 말해 요청이 섞이면 논리적 데이터도 날아갑니다.
  3. ## 4의 읽기끼리 병렬 허용, pthread_rwlock, SELECT는 read lock 결정은 현재 코드
     기준으로 너무 앞서 있습니다. 좋은 점은 새 B+Tree가 이전 구현처럼 내부 공유 캐시
     를 바꾸지 않는 순수 포인터 탐색이라 src/bptree.c:78 “미래에는” read lock 모델이
     가능해졌다는 점입니다. 하지만 지금은 읽기 요청도 전역 table_runtime_active를 보
     고, 다른 요청이 테이블을 바꾸면 런타임 자체가 해제됩니다. src/
     table_runtime.c:249 그래서 현재 안전한 기준은 global mutex이지 table rwlock이
     아닙니다.
  4. 쓰기끼리는 한번에 하나라는 결론은 맞지만, 이유가 바뀌어야 합니다. 지금은 클라이
     언트가 같은 id를 동시에 넣는 문제가 본질이 아닙니다. explicit id 삽입 자체를 거
     부합니다. src/table_runtime.c:98 실제 write hazard는 next_id, row_count, rows의
     realloc, 그리고 id_index_root 갱신입니다. src/table_runtime.c:226, src/
     table_runtime.c:298
     TODO. 그럼 너가 그렇게 write hazard에 대응할 수 있도록 문서를 수정해줘.
  5. 인덱스 단위 락은 현재 구현에는 맞지 않습니다. INSERT는 인덱스만 바꾸지 않고 스
     키마 초기화 여부, row 배열, auto id, row count, B+Tree를 같이 만집니다. src/
     table_runtime.c:286 SELECT id = ?도 B+Tree에서 row index를 찾은 뒤 row 배열을
     읽습니다. src/executor.c:451, src/executor.c:233 그래서 지금 락 후보는 인덱스가
     아니라 최소 런타임 테이블 전체, 현재 코드 그대로면 사실상 엔진 전체입니다.
        TODO. 이거 기존 코드의 구조를 뜯어고쳐서 뭔가 해결할 수는 없는거야??? 안된다면, write를 위해서는 전체 DB에 아무reader도 접근하고 있지 않아야 하잖아. 그러려면.. write favored 한 구조로 락을 설계해야 하는건가????
  6. ## 5의 worker 8개 결정은 “이 머신이 8 logical CPU를 가진다”는 점에서는 설명되지
     만, 현재 엔진의 유효 병렬성과는 맞지 않습니다. 로컬에서 getconf
     _NPROCESSORS_ONLN은 8이었지만, 지금 DB 작업은 global critical section으로 보호
     해야 맞습니다. 그러면 worker 8개는 DB throughput을 8배 늘리지 않고, 대부분 lock
     대기와 컨텍스트 스위칭만 늘릴 수 있습니다.
  7. 요청 큐 32도 현재 엔진 기준으로는 낙관적입니다. 지금 SELECT는 결과를 바로 스트
     리밍하지 않고, 결과 row를 새 메모리로 한 번 더 복사해서 만든 뒤 출력합니다.
     src/executor.c:40, src/executor.c:233, src/executor.c:276 그래서 큐 깊이는 단순
     히 “worker * 4”가 아니라 “최대 동시 대기 요청 수 * SELECT 결과 메모리”로 봐야
     합니다.
  8. worker가 parse -> execute -> respond까지 직접 담당하는 결정은 MVP로는 맞습니다.
     문제는 현재 executor API가 소켓 응답에 바로 쓸 수 있는 결과를 반환하지 않고
     stdout에 직접 출력한다는 점입니다. src/executor.h:6, src/executor.c:138, src/
     executor.c:409, src/executor.c:466 즉 “worker가 응답까지 책임”은 구조적으로 옳
     지만, 그 전에 executor를 QueryResult 반환형으로 바꾸는 리팩토링이 선행되어야 합
     니다.
  9. 현재 테스트 기준으로도 DELETE는 실행 엔진에서 미지원입니다. src/executor.c:477,
     tests/run_tests.sh:54 따라서 ## 4에서 write policy를 INSERT/DELETE/UPDATE까지
     일반화해 쓰면 과한 설명이 됩니다. 지금 서버 설계 기준 write는 사실상 INSERT 하
     나입니다.

  Assumption

  - 아래 평가는 “서버가 현재 실행 경로인 src/main.c:57 + src/executor.c:486를 감싼
    다”는 전제입니다.
  - 만약 서버가 나중에 storage.c 경로를 직접 쓰도록 엔진을 다시 바꿀 계획이면,
    flock()과 파일 기반 delete/insert를 포함해 동시성 답이 다시 달라집니다.

  Revised Section 4

  - 공유 자원: 지금은 인덱스가 아니라 engine-wide singleton runtime입니다.
    table_runtime_active, rows, row_count, next_id, id_index_root 전체가 공유 자원입
    니다. src/table_runtime.c:11
  - 읽기와 쓰기 동시 허용: 현재 코드 그대로면 아니오가 맞습니다.
  - 읽기끼리 병렬 허용: 현재 코드 그대로면 아니오가 맞습니다. 다만 전역 단일 런타임
    을 없애고 “테이블별 독립 runtime + 결과 반환 API”로 바꾸면 그때는 네로 바꿀 수
    있습니다.
  - 쓰기 제어: 지금은 전역 단일 writer가 맞습니다.
  - 락 단위: 현재 상태에서는 DB 전체(global engine). 테이블 단위는 runtime registry
    로 구조를 먼저 바꾼 뒤의 2단계 목표입니다. 인덱스 단위는 현재 구현에는 부적절합
    니다.
  - correctness vs 성능: 지금은 무조건 correctness 우선입니다. 엔진 구조상 병렬성보
    다 상태 일관성이 먼저입니다.
  - queue full: 즉시 실패가 맞습니다. HTTP라면 503 Service Unavailable 후 close가 가
  - primitive: 지금은 pthread_mutex_t 1개가 맞습니다. pthread_rwlock_t는 구조 리팩토
    링 이후에만 의미가 있습니다.
    장 명확합니다.
  - SELECT read / INSERT write: “목표 상태”로는 맞지만 “지금 바로 적용할 결정”은 아
  - worker 책임: 한 worker가 request read -> parse -> execute -> respond를 끝까지 처
    닙니다.

  Revised Section 5

  - worker 수: 현재 엔진 그대로면 8 고정보다 2~4가 현실적입니다. 로컬 CPU가 8개여도
    DB 작업 자체는 전역 lock으로 직렬화해야 해서 8 worker의 실익이 작습니다.
  - 요청 큐: bounded queue는 맞습니다.
  - queue 크기: 현재 엔진이면 32보다 8~16이 더 보수적이고 안전합니다. 특히 SELECT *
    가 큰 결과를 메모리 복사하기 때문에 큐를 크게 잡는 이점이 작습니다.
  - queue full 처리: fail-fast는 맞습니다. HTTP에서는 503 Service Unavailable으로 바
    로 거절하는 쪽이 적절합니다.
  - worker 역할: read request -> parse -> execute -> serialize -> respond -> close를
    한 worker가 끝까지 맡는 모델이 맞습니다.
  - accept thread 역할: accept + enqueue + queue full 시 즉시 reject까지만 맡는 것이
    가장 단순합니다.
  - 별도 responder thread: 지금은 비추천입니다. executor가 구조화된 결과를 반환하지
    않는 상태에서 파이프라인을 더 쪼개면 복잡성만 증가합니다.
  - 가장 단순한 모델: accept thread 1개 + bounded queue 1개 + worker pool + global
    engine mutex입니다.

  권장 순서

  1. 먼저 table_runtime_active 단일 전역 구조를 유지할지 버릴지 결정합니다.
  2. 서버를 붙일 거면 executor를 stdout 출력형에서 QueryResult 반환형으로 바꿉니다.
  3. 그 전까지는 global mutex + small worker pool + fail-fast queue로 갑니다.
  4. 이후에만 per-table runtime registry + per-table rwlock을 검토합니다.

  Testing Gap

  - make tests를 시도했지만 기존 build/*.o가 현재 환경과 맞지 않아 linker가 unknown
    file type로 실패했습니다. 그래서 이번 평가는 정적 코드 리뷰 기준입니다.