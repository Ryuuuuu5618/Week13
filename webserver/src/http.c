#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ctype.h>
#include <strings.h>   // strncasecmp
#include <stdlib.h>    // strtol, malloc, free

#include "http.h"
#include "proxy.h"

// 종료 표시: main.c의 시그널 핸들러가 켠다. 켜지면 keep-alive 연결도 지금 요청까지만 처리한다.
atomic_int stop_requested = 0;
int (*others_waiting)(void) = NULL;

/* ───────────────────────── 보내기 ───────────────────────── */

// write()는 덜 쓰고 돌아올 수 있으므로, n바이트를 다 쓸 때까지 반복한다.
int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;

    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;   // 시그널 때문에 끊긴 것이면 다시 시도
            return -1;
        }
        p += w;
        left -= w;
    }
    return 0;
}

// 상태 줄 + 헤더를 보낸다. 본문은 따로 보낸다.
// keep_alive가 1이면 "연결을 계속 쓸게요", 0이면 "응답 후 닫을게요"라고 알린다.
// (keep-alive는 받는 쪽이 본문 끝을 알아야 하므로 Content-Length가 꼭 필요하다)
static int send_headers(int fd, int status, const char *reason,
                        const char *content_type, long long length, int keep_alive)
{
    char header[512];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %lld\r\n"
                       "Connection: %s\r\n"
                       "\r\n",
                       status, reason, content_type, length,
                       keep_alive ? "keep-alive" : "close");
    return write_all(fd, header, len);
}

// 오류 응답: 상태 줄 + 헤더 + 짧은 HTML 본문. 오류 뒤에는 항상 연결을 닫는다.
// (요청이 깨졌으면 다음 요청이 어디서 시작하는지도 믿을 수 없기 때문)
void send_error(int fd, int status, const char *reason)
{
    char body[256];
    int len = snprintf(body, sizeof(body),
                       "<html><body><h1>%d %s</h1></body></html>\n",
                       status, reason);
    if (send_headers(fd, status, reason, "text/html; charset=utf-8", len, 0) == 0)
        write_all(fd, body, len);
}

/* ───────────────────────── 받기 ───────────────────────── */

// 오후에 직접 짠 코드: "\r\n\r\n"이 나올 때까지 read를 반복해 헤더를 모은다.
// keep-alive를 위해 바뀐 점: 앞 요청 뒤에 딸려 온 바이트(buffered)가 이미 buf 앞에 있을 수 있어서,
// 그 뒤에 이어서 읽고, 읽기 전에 이미 헤더 끝이 있는지부터 확인한다.
// 성공: buf에 모인 바이트 수 / 상대가 끊음·에러·타임아웃: -1 / 버퍼가 꽉 참: -2
static ssize_t read_request_header(int fd, char *buf, size_t size, size_t buffered)
{
    size_t total = buffered;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n") != NULL)
        return total;            // 파이프라이닝: 다음 요청이 이미 통째로 와 있었음

    while (total < size - 1)
    {
        ssize_t n = read(fd, buf + total, (size - 1) - total);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            // 타임아웃(EAGAIN)이나 손님이 끊은 경우(ECONNRESET)는 흔한 일이라 조용히 넘어간다
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNRESET)
                perror("read");
            return -1;
        }

        total += n;
        buf[total] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL)
            return total;
    }
    return -2;
}

/* ───────────────────────── 해석하기 ───────────────────────── */

// "GET /index.html HTTP/1.1\r\n..." → method, uri, version 으로 나눈다.
// 성공: 0 / 형식이 이상함: -1
int parse_request_line(const char *buf, struct request_line *rl)
{
    // 첫 줄의 끝(\r\n)을 찾는다.
    const char *eol = strstr(buf, "\r\n");
    if (eol == NULL)
        return -1;

    size_t line_len = eol - buf;
    char line[1100];
    if (line_len >= sizeof(line))
        return -1;   // 첫 줄이 너무 김
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    // 공백으로 나뉜 세 덩어리를 꺼낸다. %15s 처럼 최대 길이를 적어 버퍼 넘침을 막는다.
    char extra[2];
    int count = sscanf(line, "%15s %1023s %15s %1s",
                       rl->method, rl->uri, rl->version, extra);
    if (count != 3)
        return -1;   // 덩어리가 3개가 아니면 잘못된 요청

    return 0;
}

// 헤더에서 "Content-Length: 숫자"를 찾아 그 숫자를 돌려준다.
// 없으면 0, 숫자가 이상하면 -1
long get_content_length(const char *buf)
{
    const char *line = strstr(buf, "\r\n");      // 첫 줄(요청 라인)은 건너뜀
    while (line != NULL)
    {
        line += 2;                               // "\r\n" 다음 줄의 시작
        if (strncmp(line, "\r\n", 2) == 0)       // 빈 줄 = 헤더 끝
            break;

        // 헤더 이름은 대소문자를 구분하지 않으므로 strncasecmp
        if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
            char *end;
            long len = strtol(line + 15, &end, 10);   // 문자열 → 숫자
            if (end == line + 15 || len < 0)
                return -1;                            // 숫자가 없거나 음수
            return len;
        }
        line = strstr(line, "\r\n");             // 다음 줄로
    }
    return 0;
}

// 헤더에서 name의 값을 찾아 out에 복사한다 (앞뒤 공백 제거). 찾으면 0, 없으면 -1
int get_header(const char *buf, const char *name, char *out, size_t outsize)
{
    size_t name_len = strlen(name);
    const char *line = strstr(buf, "\r\n");      // 첫 줄(요청 라인)은 건너뜀
    while (line != NULL)
    {
        line += 2;
        if (strncmp(line, "\r\n", 2) == 0)       // 빈 줄 = 헤더 끝
            break;

        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':')
        {
            const char *v = line + name_len + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            const char *eol = strstr(v, "\r\n");
            size_t len = (eol != NULL) ? (size_t)(eol - v) : strlen(v);
            while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t'))
                len--;
            if (len >= outsize)
                len = outsize - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 0;
        }
        line = strstr(line, "\r\n");
    }
    return -1;
}

// 헤더 값에 token이 들어 있나 (대소문자 무시). 예: "Keep-Alive, Upgrade"에 "keep-alive"
static int has_token(const char *value, const char *token)
{
    char lower[256];
    size_t i;
    for (i = 0; value[i] != '\0' && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)value[i]);
    lower[i] = '\0';
    return strstr(lower, token) != NULL;
}

// 이 요청 뒤에 연결을 유지할지 정한다. 유지 1, 닫기 0
//  - "Connection: close"가 있으면 닫는다
//  - HTTP/1.1은 기본이 유지
//  - HTTP/1.0은 기본이 닫기, "Connection: keep-alive"가 있을 때만 유지
int wants_keep_alive(const char *buf, const char *version)
{
    char value[256] = "";
    int has = (get_header(buf, "Connection", value, sizeof(value)) == 0);

    if (has && has_token(value, "close"))
        return 0;
    if (strcmp(version, "HTTP/1.1") == 0)
        return 1;
    return has && has_token(value, "keep-alive");
}

// 파일 확장자를 보고 Content-Type 값을 정한다.
const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return "application/octet-stream";

    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";

    return "application/octet-stream";   // 모르는 종류는 "그냥 바이트"
}

/* ───────────────────────── 정적 파일 서빙 ───────────────────────── */

// 성공적으로 다 보냈으면 1, 오류 응답을 보냈거나 중간에 실패했으면 0 (→ 연결 닫기)
static int serve_static(int fd, const char *root, char *uri, int keep_alive)
{
    // 1. "?a=1" 같은 쿼리 문자열은 파일 경로가 아니므로 잘라 낸다.
    char *q = strchr(uri, '?');
    if (q != NULL)
        *q = '\0';

    // 2. 루트 바깥으로 나가려는 경로를 막는다. 예: /../../etc/passwd
    if (uri[0] != '/' || strstr(uri, "..") != NULL)
    {
        send_error(fd, 403, "Forbidden");
        return 0;
    }

    // 3. 실제 파일 경로 만들기: 루트 + uri. "/"로 끝나면 index.html을 붙인다.
    char path[2048];
    const char *suffix = (uri[strlen(uri) - 1] == '/') ? "index.html" : "";
    int len = snprintf(path, sizeof(path), "%s%s%s", root, uri, suffix);
    if (len < 0 || (size_t)len >= sizeof(path))
    {
        send_error(fd, 414, "URI Too Long");
        return 0;
    }

    // 4. 파일이 있는지, 일반 파일인지 확인한다.
    struct stat st;
    if (stat(path, &st) < 0)
    {
        send_error(fd, 404, "Not Found");
        return 0;
    }
    if (!S_ISREG(st.st_mode))
    {
        send_error(fd, 403, "Forbidden");   // 폴더 등은 보여 주지 않음
        return 0;
    }

    // 5. 파일 열기
    int filefd = open(path, O_RDONLY);
    if (filefd < 0)
    {
        if (errno == EACCES)
            send_error(fd, 403, "Forbidden");
        else
            send_error(fd, 500, "Internal Server Error");
        return 0;
    }

    // 6. 헤더를 보내고, 파일을 조금씩 읽어서 그대로 소켓에 쓴다.
    int ok = 0;
    if (send_headers(fd, 200, "OK", mime_type(path), (long long)st.st_size, keep_alive) == 0)
    {
        char chunk[8192];
        ssize_t n;
        ok = 1;
        while ((n = read(filefd, chunk, sizeof(chunk))) > 0)
        {
            if (write_all(fd, chunk, n) < 0)
            {
                ok = 0;   // 손님이 중간에 끊음
                break;
            }
        }
    }

    close(filefd);   // 파일 fd도 꼭 닫는다 (안 닫으면 fd 누수)
    return ok;
}

/* ───────────────────────── 연결 하나 처리 (keep-alive) ───────────────────────── */

// 요청 하나를 처리한다.
// buf에는 read_request_header가 모은 n바이트가 있다 (헤더 + 딸려 온 본문 + 다음 요청 일부일 수 있음).
// *consumed에 이 요청이 buf에서 쓴 바이트 수(헤더 + buf 안의 본문)를 적는다.
// 연결을 유지해도 되면 1, 닫아야 하면 0을 돌려준다.
static int handle_one_request(int connfd, const char *root, char *buf, size_t n,
                              size_t *consumed, int is_last)
{
    *consumed = n;   // 기본값: 전부 썼다고 보고, 연결을 닫는 경로에서는 상관없음

    struct request_line rl;
    if (parse_request_line(buf, &rl) < 0)
    {
        send_error(connfd, 400, "Bad Request");
        return 0;
    }
    printf("  %s %s %s\n", rl.method, rl.uri, rl.version);

    if (strcmp(rl.version, "HTTP/1.0") != 0 && strcmp(rl.version, "HTTP/1.1") != 0)
    {
        send_error(connfd, 505, "HTTP Version Not Supported");
        return 0;
    }

    // 헤더 길이와, 헤더를 읽을 때 딸려 온 본문 바이트 수
    char *header_end = strstr(buf, "\r\n\r\n");
    size_t header_len = (header_end - buf) + 4;

    long content_length = get_content_length(buf);
    if (content_length < 0)
    {
        send_error(connfd, 400, "Bad Request");
        return 0;
    }
    size_t already = n - header_len;
    if (already > (size_t)content_length)
        already = content_length;          // 그 뒤의 바이트는 다음 요청의 시작
    *consumed = header_len + already;

    // 연결을 유지할지: 손님의 Connection 헤더 + 버전 기준.
    // 서버가 꺼지는 중이거나 이 연결의 마지막 요청이면 닫는다고 미리 알린다.
    // 다른 연결이 작업 큐에서 기다리고 있으면 이 연결을 닫고 워커를 양보한다 (적응형 keep-alive).
    int keep_alive = wants_keep_alive(buf, rl.version) && !atomic_load(&stop_requested) && !is_last
                     && !(others_waiting != NULL && others_waiting());

    // 라우팅 규칙에 맞는 경로면 백엔드로 넘긴다 (Reverse Proxy)
    // 백엔드 응답을 그대로 중계하고 "백엔드가 닫으면 끝"으로 판단하므로, 프록시 요청 뒤에는 연결을 닫는다.
    struct route *r = proxy_match(rl.uri);
    if (r != NULL)
    {
        proxy_forward(connfd, r, buf, header_len, header_end + 4, already, content_length);
        return 0;
    }

    if (strcmp(rl.method, "POST") == 0)
    {
        if (content_length > 1024 * 1024)
        {
            send_error(connfd, 413, "Payload Too Large");
            return 0;
        }

        // 본문 버퍼를 만들고, 이미 온 만큼 먼저 복사
        char *body = malloc(content_length + 1);
        if (body == NULL)
        {
            send_error(connfd, 500, "Internal Server Error");
            return 0;
        }
        memcpy(body, header_end + 4, already);

        // 남은 만큼 read 반복
        size_t got = already;
        while (got < (size_t)content_length)
        {
            ssize_t rd = read(connfd, body + got, content_length - got);
            if (rd < 0 && errno == EINTR)
                continue;
            if (rd <= 0)
            {
                free(body);      // 중간에 끊겨도 메모리는 돌려준다
                return 0;
            }
            got += rd;
        }
        body[content_length] = '\0';

        // 테스트용 응답: 받은 본문을 그대로 돌려준다
        printf("  본문 %ld바이트: %s\n", content_length, body);
        int ok = send_headers(connfd, 200, "OK", "text/plain; charset=utf-8",
                              content_length, keep_alive) == 0 &&
                 write_all(connfd, body, content_length) == 0;
        free(body);
        return ok && keep_alive;
    }

    if (strcmp(rl.method, "GET") != 0)
    {
        send_error(connfd, 501, "Not Implemented");
        return 0;
    }

    return serve_static(connfd, root, rl.uri, keep_alive) && keep_alive;
}

static void set_read_timeout(int fd, int sec)
{
    struct timeval tv = {sec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// 연결 하나에서 요청을 반복해서 처리한다 (keep-alive).
// 손님이 닫자고 하거나, 오류가 나거나, 유휴 시간이 지나거나, 최대 요청 수에 닿으면 끝난다.
void handle_client(int connfd, const char *root)
{
    char buf[REQ_BUF_SIZE];
    size_t buffered = 0;     // 앞 요청 뒤에 딸려 와서 아직 처리 안 한 바이트 수

    for (int served = 0; served < MAX_KEEPALIVE_REQUESTS; served++)
    {
        // 두 번째 요청부터는 "다음 요청을 기다리는 시간"을 짧게 잡는다
        // (keep-alive 연결이 워커를 오래 붙잡지 않도록)
        if (served > 0)
            set_read_timeout(connfd, KEEPALIVE_IDLE_SEC);

        ssize_t n = read_request_header(connfd, buf, sizeof(buf), buffered);
        if (n == -1)
            return;   // 손님이 닫았거나, 에러이거나, 유휴 시간 초과
        if (n == -2)
        {
            send_error(connfd, 431, "Request Header Fields Too Large");
            return;
        }

        if (served > 0)
            set_read_timeout(connfd, CLIENT_TIMEOUT_SEC);   // 요청을 받는 동안은 원래 시간으로

        size_t consumed;
        int is_last = (served == MAX_KEEPALIVE_REQUESTS - 1);
        int keep = handle_one_request(connfd, root, buf, (size_t)n, &consumed, is_last);
        if (!keep)
            return;

        // 이 요청이 쓰고 남은 바이트(다음 요청의 앞부분)를 buf 맨 앞으로 옮긴다
        buffered = (size_t)n - consumed;
        memmove(buf, buf + consumed, buffered);
    }
}
