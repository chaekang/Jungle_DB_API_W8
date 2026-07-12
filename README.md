# Jungle DB API

C99로 구현한 미니 DBMS HTTP API 서버입니다. `POST /query` 하나의 엔드포인트로 SQL을 받아 메모리 기반 테이블 런타임에서 실행하고, 필요한 데이터는 `data/*.csv`로 저장합니다.

이 저장소는 비싼 관리형 DB 없이 Docker 컨테이너 하나로 배포할 수 있게 구성되어 있습니다. 작은 VPS, 개인 서버, 수업용 클라우드 VM에서 낮은 비용으로 운영하는 것을 기본값으로 잡았습니다.

## 주요 기능

- `INSERT`, `SELECT`, 단일 `WHERE` 조건 일부 구문 지원
- `POST /query` JSON API 제공
- worker thread + bounded queue 기반 동시 요청 처리
- 테이블 단위 `pthread_rwlock_t`로 읽기/쓰기 동시성 제어
- `data/` 볼륨을 통한 CSV 데이터 영속화
- Docker healthcheck, non-root 실행, 낮은 리소스 제한 포함

## 빠른 실행

### Docker Compose

```bash
docker compose up -d --build api
```

기본 포트는 `8080`입니다.

```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  --data-binary @- <<'JSON'
{"sql":"INSERT INTO users (name, age) VALUES ('Alice', 30);"}
JSON
```

```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT * FROM users;"}'
```

응답 예시는 다음과 같습니다.

```json
{
  "success": true,
  "columns": ["id", "name", "age"],
  "rows": [["1", "Alice", "30"]]
}
```

다른 호스트 포트를 쓰고 싶으면 `API_PORT`를 지정합니다.

```bash
API_PORT=18080 docker compose up -d --build api
```

### npm 스크립트

Node 앱은 아니지만, 팀원이 같은 명령을 쓰기 쉽도록 `package.json`에 실행 스크립트를 넣었습니다.

```bash
npm run docker:up
npm run docker:logs
npm run docker:down
npm run docker:test
```

새 이미지까지 다시 빌드해서 배포할 때는 다음 명령을 사용합니다.

```bash
npm run docker:deploy
```

## 배포 방법

가장 저렴하고 단순한 운영 방식은 Docker가 설치된 작은 VM 한 대에 올리는 것입니다. 별도 DB 서버가 필요 없고, 데이터는 Docker volume인 `db-data`에 저장됩니다.

```bash
git clone <your-repository-url>
cd Jungle_DB_API_W8
docker compose up -d --build api
docker compose logs -f api
```

운영 기본값은 다음과 같습니다.

| 항목 | 기본값 |
| --- | --- |
| 컨테이너 포트 | `8080` |
| 호스트 포트 | `${API_PORT:-8080}` |
| CPU 제한 | `0.50` |
| 메모리 제한 | `256m` |
| 재시작 정책 | `unless-stopped` |
| 데이터 볼륨 | `db-data:/app/data` |

세부 운영값은 [docker-compose.yml](./docker-compose.yml)을 기준으로 관리합니다.

HTTPS와 도메인이 필요하면 이 컨테이너 앞에 Caddy, Nginx Proxy Manager, Cloudflare Tunnel 같은 얇은 프록시를 붙이면 됩니다. API 서버 자체는 내부 포트 `8080`만 열어두는 구성이 가장 단순합니다.

## 로컬 개발

Linux 또는 WSL 환경에서 빌드합니다.

```bash
make
./sql_processor --server 8080
```

`package.json`을 사용할 경우 로컬 명령은 Linux/WSL 기준의 `local:*` 스크립트로 분리되어 있습니다.

```bash
npm run local:build
npm run local:test
npm run local:start
```

테스트는 다음 명령으로 실행합니다.

```bash
make tests
```

Docker 안에서 테스트하려면 다음을 사용합니다.

```bash
docker compose --profile test run --rm test
```

정리 명령은 다음과 같습니다.

```bash
make clean
docker compose down --volumes --remove-orphans
```

## API

### `POST /query`

요청 body는 JSON이며 `sql` 문자열 필드가 필요합니다.

```json
{
  "sql": "SELECT * FROM users;"
}
```

성공 응답은 메시지 또는 테이블 형태로 내려옵니다.

```json
{
  "success": true,
  "message": "1 row inserted into users."
}
```

```json
{
  "success": true,
  "columns": ["id", "name", "age"],
  "rows": [["1", "Alice", "30"]]
}
```

에러 응답은 다음 형식입니다.

```json
{
  "success": false,
  "error": "Table 'users' not found."
}
```

## 지원 SQL 예시

```sql
INSERT INTO users (name, age) VALUES ('Alice', 30);
INSERT INTO users (name, age) VALUES ('Bob', 25);
SELECT * FROM users;
SELECT name, age FROM users WHERE age > 27;
SELECT * FROM users WHERE name = 'Bob';
```

`DELETE`는 parser 단계에서는 인식하지만, 현재 메모리 런타임 실행 모드에서는 지원하지 않습니다.

## 프로젝트 구조

```text
src/                 C 소스 코드
tests/               단위/통합 테스트
loadtest/k6/         k6 부하 테스트 스크립트
docs/                설계, 실험, 발표용 문서
data/                런타임 CSV 데이터 저장 위치
Dockerfile           멀티스테이지 배포 이미지
docker-compose.yml   로컬/서버 배포 구성
package.json         공통 실행 스크립트
```

## 운영 메모

- Docker healthcheck는 컨테이너의 메인 서버 프로세스가 살아 있는지 확인합니다. HTTP 레벨 readiness가 필요하면 서버에 별도 `/healthz` 엔드포인트를 추가하는 것이 좋습니다.
- Compose의 `read_only: true` 때문에 컨테이너 루트 파일시스템은 읽기 전용이고, 데이터는 `/app/data` 볼륨에만 기록됩니다.
- 현재 worker 수와 queue 크기는 코드 상수로 고정되어 있습니다. 트래픽이 늘면 `src/server.c`의 `SERVER_WORKER_COUNT`, `SERVER_QUEUE_CAPACITY`를 조정한 뒤 다시 빌드하세요.
