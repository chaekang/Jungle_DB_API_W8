# 미니 DBMS - API 서버 설계 질문 리스트

## 목적
오늘 구현할 **미니 DBMS - API 서버**의 설계를 빠르게 정리하기 위해,
팀이 먼저 답해야 할 질문들을 범주별로 모아둔 문서다.

---

## 1. 범위부터 고정하는 질문

- 오늘 반드시 동작해야 하는 시나리오는 무엇인가?
  - 예: `CREATE TABLE`, `INSERT`, `SELECT`, `DELETE`
- SQL 문법은 어디까지 지원할 것인가?
  - `WHERE id = ?`만 지원할 것인가?
  - 다른 필드 조건도 지원할 것인가?
  - `UPDATE`도 포함할 것인가?
- API는 SQL 문자열을 그대로 받게 할 것인가, 아니면 기능별 endpoint로 나눌 것인가?
  - `/insert`, `/select`처럼 기능별 분리로 가자.
  이유: SQL을 그대로 받게 하면 생기는 문제
    1.  보안(SQL injection) 문제, 
    2. 권한 제어가 어렵다,  
    3. DB 구조가 API 바깥으로 공개됨(보안2), 
    4. DB 바꾸기 어려움

- 이번 과제의 핵심은 무엇인가?
  - API 서버
  - 동시성 처리
- 우리 팀의 MVP는 정확히 무엇인가?
  - API 서버
  - 동시성 처리
---

## 2. 외부 API와 내부 DB 엔진 경계를 묻는 질문

- API 서버는 SQL 문자열만 받아서 엔진에 넘기면 끝인가?
- 파싱, 실행, 결과 포맷팅 중 어디까지를 DB 엔진의 책임으로 둘 것인가?
- 엔진의 입력과 출력 인터페이스는 어떻게 정의할 것인가?
  - 입력: SQL 문자열?
  - 출력: 성공/실패, row 집합, 에러 메시지?
- API 서버는 엔진 결과를 JSON으로만 바꿔주는 얇은 계층으로 둘 것인가?
- DB 엔진은 HTTP를 전혀 모르도록 분리할 것인가?
- HTTP 계층을 떼어내도 DB 엔진이 그대로 동작하는가?

---

## 3. 요청 처리 흐름을 묻는 질문

- 클라이언트 요청 1개가 들어오면 정확히 어떤 단계를 거치는가?
  - `accept`
  - request parse
  - thread pool queue 삽입
  - worker thread가 요청 처리
  - SQL 실행
  - 결과 직렬화
  - 응답 전송
- 어디에서 실패할 수 있는가?
  - 잘못된 HTTP
  - 잘못된 JSON
  - 잘못된 SQL
  - 테이블 없음
  - 락 충돌
  - 파일 쓰기 실패
- 각 실패를 어떤 HTTP status와 어떤 에러 메시지로 보낼 것인가?
- 이 요청 흐름을 시퀀스 다이어그램으로 그리면 어떻게 되는가?

---

## 4. 동시성에서 가장 먼저 물어야 할 질문

- 동시에 여러 요청이 오면 어떤 데이터가 공유 자원인가?
  - 테이블 데이터
  - 인덱스
  - 파일
  - 메모리 버퍼
- 읽기와 쓰기를 동시에 허용할 것인가?
- 읽기끼리는 병렬 허용할 것인가?
- 쓰기끼리는 어떻게 제어할 것인가?
- 락 단위는 어디까지로 할 것인가?
  - DB 전체
  - 테이블 단위
  - 인덱스 단위
- correctness를 우선할 것인가, 성능을 우선할 것인가?
- 처음 구현은 global mutex로 단순하게 갈 것인가, `pthread_rwlock`까지 갈 것인가?
- `SELECT`는 read lock, `INSERT/UPDATE/DELETE/CREATE`는 write lock으로 갈 것인가?

---

## 5. 스레드 풀 자체에 대해 던질 질문

- worker thread는 몇 개 둘 것인가?
  - CPU 코어 수 기준?
  - 고정 4개 / 8개?
- 요청 큐는 bounded queue로 둘 것인가?
- 큐가 가득 차면 어떻게 처리할 것인가?
  - 대기?
  - 즉시 실패 응답?
- 스레드가 직접 소켓 응답까지 보낼 것인가, 아니면 결과만 넘길 것인가?
- worker 하나가 요청 하나를 parse → execute → respond까지 끝까지 처리하는 모델로 갈 것인가?
- 디버깅과 구현 단순성을 위해 가장 단순한 모델은 무엇인가?

---

## 6. API 모양을 정할 때 던질 질문

- API는 REST처럼 보이게 만들 것인가, SQL gateway처럼 만들 것인가?
- 가장 단순한 요청/응답 포맷은 무엇인가?
- `POST /query` 하나로 처리하면 기존 SQL 처리기를 가장 쉽게 재사용할 수 있는가?
- 기능별 endpoint로 나누면 무엇이 좋아지고, 무엇이 복잡해지는가?
- 요청 body는 어떤 형식으로 받을 것인가?

예시:

```json
{ "sql": "SELECT * FROM users WHERE id = 1;" }
```

- 응답은 어떤 형식으로 줄 것인가?
  - success
  - rows
  - error
  - execution time
  - index used 여부

---

---

## 8. 테스트 설계를 위해 던질 질문

- 단위 테스트는 어디를 검증할 것인가?
  - SQL parser
  - executor
  - B+Tree lookup
  - thread-safe queue
- 통합 테스트는 어떤 시나리오로 검증할 것인가?
  - 단일 요청 성공
  - 잘못된 SQL 실패
  - 동시 `SELECT`
  - `SELECT + INSERT` 동시 실행
- 엣지 케이스는 무엇인가?
  - 빈 결과
  - 없는 테이블
  - 중복 ID
  - 긴 SQL
  - malformed JSON
  - queue full
- 데모 때 보여줄 정상 시나리오 3개와 실패 시나리오 3개는 무엇인가?

---

## 10. 설계를 시작할 때 반드시 답해야 하는 10개

1. MVP SQL 범위는 어디까지인가?
2. API는 `/query` 하나로 갈 것인가?
3. DB 엔진과 API 서버 경계는 어떻게 나눌 것인가?
4. worker thread 하나가 요청을 끝까지 처리하는가?
5. thread pool 크기와 queue 크기는 얼마로 할 것인가?
6. 동시성 제어는 global mutex인가, rwlock인가?
7. 읽기-읽기 병렬은 허용할 것인가?
8. 데이터는 메모리만 쓸 것인가, 파일에도 저장할 것인가?
9. 에러 응답 포맷은 어떻게 통일할 것인가?
10. 테스트 및 데모 시나리오는 무엇인가?

---

## 11. 팀이랑 바로 얘기할 때 쓸 핵심 질문 세트

- 오늘 끝낼 MVP 기능은 정확히 뭐야?
- 우리는 SQL gateway 방식으로 갈까, REST endpoint로 쪼갤까?
- 기존 SQL 처리기를 거의 안 건드리고 재사용하려면 인터페이스를 어떻게 잡아야 해?
- 동시성 제어는 어디에 걸 거야? DB 전체? 테이블 단위?
- 읽기와 쓰기가 동시에 들어오면 어떤 정책으로 처리할 거야?
- 스레드 풀 worker가 요청 하나를 끝까지 처리하게 할 거야?
- 데이터를 재시작 후에도 남길 거야?
- 에러 응답을 어떻게 표준화할 거야?
- 테스트는 어떤 시나리오를 반드시 통과해야 해?
- 우리 팀만의 차별점 하나는 뭐로 가져갈 거야?

---

## 12. 현실적인 MVP 방향 예시

- API: `POST /query`
- 요청 body: `{ "sql": "..." }`
- main thread는 `accept + queue push`
- worker thread가 요청을 꺼내 처리
- worker가 SQL 실행 후 JSON 응답 반환
- 동시성 제어:
  - DB 전체에 `pthread_rwlock_t`
  - `SELECT`는 read lock
  - `INSERT/UPDATE/DELETE/CREATE`는 write lock
- 저장:
  - 데이터 파일 저장
  - 인덱스는 시작 시 rebuild
- 테스트:
  - engine unit test
  - API integration test
  - concurrent request test
- 차별점:
  - `execution_time_ms`
  - `index_used: true/false`
  - `/health`, `/metrics`

---

## 한 줄 정리

설계를 시작할 때 가장 먼저 물어야 할 것은 다음이다.

> **우리가 오늘 끝낼 MVP는 무엇이고, 그 MVP를 가장 단순하면서도 설명 가능하게 만들 구조는 무엇인가?**
