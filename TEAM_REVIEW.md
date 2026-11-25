# 13주차 C 웹서버 팀 리뷰

POSIX 소켓으로 HTTP 요청을 받고 정적 파일, `POST /echo`, 포워드·리버스 프록시를 처리하는 개인 구현안이다. 라운드 로빈과 두 프록시의 캐시까지 검증했다. 팀 리뷰에서는 이 결과를 재현하고, 병합본에 어떤 구조를 채택할지 결정한다.

## 가져갈 파일

| 파일 | 역할 |
| --- | --- |
| [server.c](server.c) | TCP 연결, HTTP 파싱·응답, 파일 서빙, 동시 처리, 프록시·캐시 |
| [Makefile](Makefile) | `make`로 `webserver` 실행 파일 빌드 |
| [public/](public/) | 정적 파일 테스트에 쓰는 `index.html`, `logo.png` |
| [학습 자료](csapp11-web-server.html) | 06 통합 검증, 07 빌드·단위 검증, 08 라운드 로빈, 09 캐싱 |
| [10 과제 명세 복습](csapp11-web-server.html#requirements-review) | 명세의 용어, 구현 범위와 검증 근거를 확인하는 팀 리뷰 자료 |

공유할 때는 소스와 파일을 포함하고, 실행 파일 `webserver`는 팀원 환경에서 `make`로 생성한다. 이 문서는 개인 구현의 리뷰 기록이다. 발표에 사용할 최종 README는 팀이 병합한 코드의 명령과 결과에 맞춘다.

## 1. 기본 서버 실습

WSL Ubuntu에서 프로젝트 폴더를 열고 실행한다. `Listening`이 뜨면 별도 터미널에서 요청을 보낸다.

```bash
make
./webserver --pthread --port 8090 --root public
```

```bash
curl --noproxy 127.0.0.1 -i -w '\n' http://127.0.0.1:8090/
curl --noproxy 127.0.0.1 -i http://127.0.0.1:8090/index.html
curl --noproxy 127.0.0.1 -i -w '\n' --data-binary 'jungle' http://127.0.0.1:8090/echo
curl --noproxy 127.0.0.1 -i http://127.0.0.1:8090/missing.html
```

차례로 `200`과 `hello`, `200`과 HTML, `200`과 `jungle`, `404`와 빈 본문을 예상한다. 소스를 그대로 두고 `make`를 다시 실행하면 최신 상태라 빌드를 건너뛴다. 실습을 마치면 서버 터미널에서 `Ctrl+C`로 종료한다.

## 2. 라운드 로빈 재현

포트 8001·8002·8080이 비어 있는 상태에서 터미널 네 개를 프로젝트 폴더로 열어 순서대로 실행한다. 터미널 1~3의 서버를 켜 둔 채 터미널 4에서 요청한다.

**터미널 1 · 8001 백엔드**

```bash
make
root_a=$(mktemp -d)
mkdir -p "$root_a/api"
printf '%s' 'hello A' > "$root_a/api/hello"
./webserver --port 8001 --root "$root_a"
```

**터미널 2 · 8002 백엔드**

```bash
root_b=$(mktemp -d)
mkdir -p "$root_b/api"
printf '%s' 'hello B' > "$root_b/api/hello"
./webserver --port 8002 --root "$root_b"
```

**터미널 3 · 8080 리버스 프록시**

```bash
./webserver --pthread --port 8080 --reverse /api/=8001,8002
```

**터미널 4 · 같은 요청 네 번**

```bash
for i in 1 2 3 4; do
  curl --noproxy 127.0.0.1 -fsS -w '\n' http://127.0.0.1:8080/api/hello
done
```

개인 WSL 실측 결과는 `hello A → hello B → hello A → hello B`였다. 네 요청은 모두 8080에 갔고, 프록시가 연결하는 백엔드만 번갈아 바뀌었다. 끝나면 세 서버 창에서 각각 `Ctrl+C`를 누른다.

## 3. 포워드·리버스 캐시 재현

아래 서버는 요청이 원 서버까지 도착했을 때만 응답 번호를 올린다. 각 실험을 새로 시작하면 원 서버 카운트와 프록시 캐시가 초기 상태다. 터미널 1~3을 켜 둔 채 터미널 4에서 실행한다.

**터미널 1 · 원 서버**

```bash
make
python3 - <<'PY'
from http.server import BaseHTTPRequestHandler, HTTPServer

class Origin(BaseHTTPRequestHandler):
    count = 0

    def do_GET(self):
        Origin.count += 1
        body = f"origin response {Origin.count}".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "public, max-age=30")
        self.end_headers()
        self.wfile.write(body)

HTTPServer(("127.0.0.1", 8001), Origin).serve_forever()
PY
```

**터미널 2 · 8081 포워드 프록시**

```bash
./webserver --pthread --port 8081 --forward --cache
```

**터미널 3 · 8080 리버스 프록시**

```bash
./webserver --pthread --port 8080 --reverse /api/=8001 --cache
```

**터미널 4 · 캐시 적중과 만료 확인**

```bash
for i in 1 2; do
  curl --noproxy '' --proxy http://127.0.0.1:8081 -i -w '\n' http://127.0.0.1:8001/api/hello
done
for i in 1 2; do
  curl --noproxy 127.0.0.1 -i -w '\n' http://127.0.0.1:8080/api/hello
done
sleep 31
curl --noproxy 127.0.0.1 -i -w '\n' http://127.0.0.1:8080/api/hello
```

| 개인 WSL 결과 | 확인한 동작 |
| --- | --- |
| 포워드 요청 두 건 모두 `origin response 1`, 둘째에 `Age: 0` | 8081이 저장한 응답 재사용 |
| 리버스 요청 두 건 모두 `origin response 2`, 둘째에 `Age: 0` | 8080이 자기 캐시에서 응답 재사용 |
| 원 서버 창의 GET 로그 두 줄 | 첫 네 요청 중 두 건만 원 서버에 도착 |
| 만료 후 새 요청에 `origin response 3` | 8080이 원 서버에서 다시 받아옴 |

## 4. 검증 기록

| 항목 | 확인 결과 |
| --- | --- |
| 파서·MIME·라우팅 단위 검증 | `Host` 유무, `logo.png`·`logo.PNG`, 가장 긴 경로 규칙과 미일치 결과를 각각 함수에 입력해 확인 |
| 파일·동적 요청 | HTML·CSS·JS·PNG·JPG의 MIME 매핑, `403`·`404`·`500`, `POST /echo` 확인 |
| 동시 처리와 종료 | `fork`, `pthread`, `poll`의 동시 요청 처리와 `SIGINT`·`SIGTERM` 종료 확인 |
| 프록시 장애 | 백엔드 연결 실패 `502`, 응답 대기 시간 초과 `504`, 이후 정상 요청 복구 확인 |
| 경계 조건 | 중간에 끊긴 헤더·본문 정리, 미지원 메서드·버전, 본문 `413`과 요청 대상 `414` 확인 |
| 메모리 검사 | WSL의 정적·POST·오류 실습과 별도 환경의 프록시·라운드 로빈·캐시 실험에서 AddressSanitizer·LeakSanitizer 보고 없음 |
| 부하 테스트 | WSL의 8080 `/index.html`에 200건씩 실행. 동시 1개는 439.60건/초·중앙값 2ms, 동시 8개는 1,174.07건/초·중앙값 7ms. 양쪽 실패 0건 |

위 수치는 개인 구현을 검증한 결과다. 부하 테스트는 WSL 로컬의 정적 파일 실험이고, 병합본에서 측정하면 수치가 달라질 수 있다. 자세한 실습 흐름은 [HTML 학습 자료](csapp11-web-server.html)에 정리했다.

## 5. 팀에서 결정할 내용

- **동시 처리.** 현재 프록시 실습은 `pthread` 워커 두 개를 사용한다. 한 워커가 백엔드를 기다릴 때 비어 있는 워커가 다른 요청을 맡을 수 있고, 두 워커가 라운드 로빈 순번과 캐시를 공유한다. 현재 `poll` 모드는 프록시 중계 중 이벤트 루프도 기다린다. `fork` 모드에서 여러 백엔드 라운드 로빈과 캐시를 쓰려면 프로세스 사이에 상태를 공유하는 설계가 필요하다.
- **HTTP 범위.** `GET`·`POST`와 `Content-Length` 기반 요청 본문을 지원한다. 응답 후 연결을 닫는다. 포워드 프록시 실습은 일반 HTTP이고, `CONNECT` HTTPS 터널링과 청크 전송은 지원 범위 밖이다. 로컬 실습 서버는 `127.0.0.1`에서 연결을 받는다.
- **분배와 장애.** 경로의 가장 긴 접두사 규칙을 고르고 그 규칙에 속한 백엔드만 순환한다. 순번 잠금은 선택 직후 해제한다. 선택한 백엔드 연결이 실패하면 그 요청에 `502`를 돌려주고 다음 요청을 다음 후보에 배정한다.
- **캐시 정책.** `--cache`를 켜고 원 서버가 명시적으로 재사용을 허용한 공개 GET의 200 응답만 보관한다. 최대 16개, 본문 64 KiB, 보관 60초 한도를 둔다. 인증·쿠키가 있는 요청과 `Set-Cookie`·`Vary`가 있는 응답은 건너뛴다. 리버스 캐시 적중 시 백엔드를 고르지 않으므로 라운드 로빈 순번도 그대로다.
- **병합 검증.** 팀이 지원할 모드·옵션과 테스트 범위를 결정한다. 병합 후 `make`와 기본 HTTP 동작, 두 프록시, 분배, 캐시 재사용·만료, 종료 및 메모리 검사를 다시 실행해 최종 README에 결과를 남긴다.

## 6. 첫 팀 리뷰 진행 순서

1. **개인 구현 비교.** 각자 빌드·실행 방법과 검증 결과를 공유한다. TCP 연결만 됐을 때, 요청 일부를 읽었을 때, HTTP 응답을 받은 때를 로그로 구분한다.
2. **채택할 동작 결정.** 동시 처리 방식, 정적 파일 루트, 프록시 실행 옵션, 라운드 로빈·캐시 정책을 합의한다. 따로 검토할 학습 질문은 [10 과제 명세 복습](csapp11-web-server.html#requirements-review)에 모여 있다.
3. **팀 코드에 병합.** 팀원이 작업한 브랜치와 병합 대상 브랜치를 확인한 뒤 합친다. 빌드나 CLI의 충돌이 있으면 실행 방법도 함께 정리한다.
4. **병합본 재검증.** 먼저 기본 요청 결과를 확인하고, 라운드 로빈의 `A → B → A → B`, 캐시 적중·만료, 종료를 재현한다. 팀에서 실제로 실행한 명령과 결과를 최종 README에 옮긴다.

과제 근거는 [웹 서버 요구사항](WEB_SERVER_REQUIREMENTS.md)과 [13주차 가이드](WEEK13_GUIDE.md)에 있다.
