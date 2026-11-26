// 단위 테스트: 서버를 띄우지 않고 함수 하나씩 입력 → 결과를 확인한다.
// 실행: make test
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/http.h"
#include "../src/proxy.h"

static int passed = 0, failed = 0;

// 조건이 참이면 통과, 거짓이면 실패를 기록하고 어디서 틀렸는지 출력한다.
#define CHECK(cond, name)                                         \
    do {                                                          \
        if (cond) { passed++; printf("  ✓ %s\n", name); }         \
        else { failed++; printf("  ✗ %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ───────────── parse_request_line ───────────── */
static void test_parse_request_line(void)
{
    printf("[parse_request_line]\n");
    struct request_line rl;

    CHECK(parse_request_line("GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n", &rl) == 0
          && strcmp(rl.method, "GET") == 0
          && strcmp(rl.uri, "/index.html") == 0
          && strcmp(rl.version, "HTTP/1.1") == 0,
          "정상 요청을 메서드, 경로, 버전으로 나눈다");

    CHECK(parse_request_line("POST /api/echo HTTP/1.0\r\n\r\n", &rl) == 0
          && strcmp(rl.method, "POST") == 0,
          "POST와 HTTP/1.0도 나눈다");

    CHECK(parse_request_line("GET /index.html\r\n\r\n", &rl) == -1,
          "버전이 빠지면 거절 (덩어리 2개)");

    CHECK(parse_request_line("GET / HTTP/1.1 EXTRA\r\n\r\n", &rl) == -1,
          "덩어리가 4개면 거절");

    CHECK(parse_request_line("HELLO\r\n\r\n", &rl) == -1,
          "덩어리가 1개면 거절");

    CHECK(parse_request_line("GET / HTTP/1.1", &rl) == -1,
          "첫 줄 끝(\\r\\n)이 없으면 거절");

    CHECK(parse_request_line("AAAAAAAAAAAAAAAAAAAAAAAA / HTTP/1.1\r\n\r\n", &rl) == -1,
          "메서드가 15자를 넘으면 거절 (버퍼 넘침 방지)");

    char longline[1300];
    memset(longline, 'a', sizeof(longline));
    memcpy(longline, "GET /", 5);
    strcpy(longline + 1200, " HTTP/1.1\r\n\r\n");
    CHECK(parse_request_line(longline, &rl) == -1,
          "첫 줄이 너무 길면 거절");
}

/* ───────────── get_content_length ───────────── */
static void test_get_content_length(void)
{
    printf("[get_content_length]\n");

    CHECK(get_content_length("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n") == 11,
          "Content-Length 값을 꺼낸다");

    CHECK(get_content_length("POST / HTTP/1.1\r\ncontent-length: 5\r\n\r\n") == 5,
          "헤더 이름의 대소문자를 구분하지 않는다");

    CHECK(get_content_length("POST / HTTP/1.1\r\nHost: a\r\nContent-Length:   42\r\nX: y\r\n\r\n") == 42,
          "다른 헤더 사이에 있어도, 앞 공백이 있어도 찾는다");

    CHECK(get_content_length("GET / HTTP/1.1\r\nHost: a\r\n\r\n") == 0,
          "없으면 0");

    CHECK(get_content_length("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n") == -1,
          "숫자가 아니면 -1");

    CHECK(get_content_length("POST / HTTP/1.1\r\nContent-Length: -3\r\n\r\n") == -1,
          "음수면 -1");

    CHECK(get_content_length("POST / HTTP/1.1\r\nHost: a\r\n\r\nContent-Length: 99") == 0,
          "빈 줄(헤더 끝) 뒤의 본문은 헤더로 보지 않는다");
}

/* ───────────── get_header / wants_keep_alive ───────────── */
static void test_keep_alive(void)
{
    printf("[get_header / wants_keep_alive]\n");
    char v[64];

    CHECK(get_header("GET / HTTP/1.1\r\nHost:  example.com  \r\n\r\n", "Host", v, sizeof(v)) == 0
          && strcmp(v, "example.com") == 0,
          "헤더 값을 꺼내고 앞뒤 공백을 지운다");
    CHECK(get_header("GET / HTTP/1.1\r\nconnection: Close\r\n\r\n", "Connection", v, sizeof(v)) == 0
          && strcmp(v, "Close") == 0,
          "헤더 이름의 대소문자를 구분하지 않는다");
    CHECK(get_header("GET / HTTP/1.1\r\nHost: a\r\n\r\n", "Connection", v, sizeof(v)) == -1,
          "없으면 -1");
    CHECK(get_header("GET / HTTP/1.1\r\nHostname: x\r\n\r\n", "Host", v, sizeof(v)) == -1,
          "Hostname을 Host로 착각하지 않는다 (콜론까지 비교)");

    CHECK(wants_keep_alive("GET / HTTP/1.1\r\nHost: a\r\n\r\n", "HTTP/1.1") == 1,
          "HTTP/1.1은 기본이 유지");
    CHECK(wants_keep_alive("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", "HTTP/1.1") == 0,
          "HTTP/1.1이라도 Connection: close면 닫기");
    CHECK(wants_keep_alive("GET / HTTP/1.0\r\nHost: a\r\n\r\n", "HTTP/1.0") == 0,
          "HTTP/1.0은 기본이 닫기");
    CHECK(wants_keep_alive("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", "HTTP/1.0") == 1,
          "HTTP/1.0이라도 Connection: keep-alive면 유지 (대소문자 무시)");
}

/* ───────────── mime_type ───────────── */
static void test_mime_type(void)
{
    printf("[mime_type]\n");
    CHECK(strncmp(mime_type("./www/index.html"), "text/html", 9) == 0, ".html → text/html");
    CHECK(strncmp(mime_type("a.css"), "text/css", 8) == 0, ".css → text/css");
    CHECK(strncmp(mime_type("a.js"), "application/javascript", 22) == 0, ".js → application/javascript");
    CHECK(strcmp(mime_type("a.png"), "image/png") == 0, ".png → image/png");
    CHECK(strcmp(mime_type("a.jpg"), "image/jpeg") == 0, ".jpg → image/jpeg");
    CHECK(strcmp(mime_type("a.jpeg"), "image/jpeg") == 0, ".jpeg → image/jpeg");
    CHECK(strcmp(mime_type("README"), "application/octet-stream") == 0, "확장자가 없으면 octet-stream");
    CHECK(strcmp(mime_type("a.tar.gz"), "application/octet-stream") == 0, "모르는 확장자는 octet-stream (마지막 . 기준)");
    CHECK(strcmp(mime_type("./dir.v2/file.png"), "image/png") == 0, "폴더 이름의 . 에 속지 않는다");
}

/* ───────────── proxy_match (라우팅) ───────────── */
static void test_proxy_match(void)
{
    printf("[proxy_load_routes / proxy_match]\n");

    // 임시 설정 파일을 만들어서 읽는다
    char path[] = "/tmp/routes_testXXXXXX";
    int fd = mkstemp(path);
    const char *conf =
        "# 주석 줄은 무시\n"
        "\n"
        "/api/     127.0.0.1:8001 127.0.0.1:8002\n"
        "/api/v2/  127.0.0.1:8003\n"
        "/static/  localhost:9000\n";
    write(fd, conf, strlen(conf));
    close(fd);

    CHECK(proxy_load_routes(path) == 0, "설정 파일을 읽는다");
    unlink(path);

    struct route *r = proxy_match("/api/hello.txt");
    CHECK(r != NULL && strcmp(r->prefix, "/api/") == 0 && r->nbackends == 2,
          "/api/hello.txt → /api/ 라우트, 백엔드 2개");

    r = proxy_match("/api/v2/users");
    CHECK(r != NULL && strcmp(r->prefix, "/api/v2/") == 0,
          "더 길게 맞는 라우트를 고른다 (/api/v2/)");

    r = proxy_match("/static/logo.png");
    CHECK(r != NULL && strcmp(r->backends[0].host, "localhost") == 0
          && strcmp(r->backends[0].port, "9000") == 0,
          "host:port를 나눠서 저장한다");

    CHECK(proxy_match("/index.html") == NULL, "맞는 라우트가 없으면 NULL (직접 처리)");
    CHECK(proxy_match("/apix") == NULL, "/apix는 /api/와 다르다 (끝의 / 까지 비교)");
    CHECK(proxy_match("/") == NULL, "/ 는 라우트 없음");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_parse_request_line();
    test_get_content_length();
    test_keep_alive();
    test_mime_type();
    test_proxy_match();

    printf("\n결과: %d개 통과, %d개 실패\n", passed, failed);
    return failed == 0 ? 0 : 1;   // 실패가 있으면 make test도 실패로 끝난다
}
