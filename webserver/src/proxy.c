#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "proxy.h"
#include "http.h"

static struct route routes[MAX_ROUTES];
static int nroutes = 0;

/* ───────────────────────── 설정 파일 읽기 ───────────────────────── */

// 형식 (한 줄에 라우트 하나):
//   /api/  127.0.0.1:8001  127.0.0.1:8002
//   # 로 시작하는 줄과 빈 줄은 무시
int proxy_load_routes(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        perror(path);
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *save;
        char *tok = strtok_r(line, " \t\r\n", &save);
        if (tok == NULL || tok[0] == '#')
            continue;
        if (nroutes == MAX_ROUTES)
        {
            fprintf(stderr, "라우트는 최대 %d개입니다.\n", MAX_ROUTES);
            break;
        }

        struct route *r = &routes[nroutes];
        memset(r, 0, sizeof(*r));
        snprintf(r->prefix, sizeof(r->prefix), "%s", tok);

        // 나머지 토큰은 "host:port"
        while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL && r->nbackends < MAX_BACKENDS)
        {
            char *colon = strrchr(tok, ':');
            if (colon == NULL)
            {
                fprintf(stderr, "잘못된 백엔드 형식: %s (host:port)\n", tok);
                continue;
            }
            *colon = '\0';
            struct backend *b = &r->backends[r->nbackends++];
            snprintf(b->host, sizeof(b->host), "%s", tok);
            snprintf(b->port, sizeof(b->port), "%s", colon + 1);
        }

        if (r->nbackends == 0)
        {
            fprintf(stderr, "백엔드가 없는 라우트: %s\n", r->prefix);
            continue;
        }
        printf("라우트: %s → 백엔드 %d개\n", r->prefix, r->nbackends);
        nroutes++;
    }

    fclose(fp);
    return 0;
}

// 가장 길게 맞는 prefix를 고른다. 예: /api/v2/ 와 /api/ 가 둘 다 맞으면 /api/v2/
struct route *proxy_match(const char *uri)
{
    struct route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < nroutes; i++)
    {
        size_t len = strlen(routes[i].prefix);
        if (strncmp(uri, routes[i].prefix, len) == 0 && len > best_len)
        {
            best = &routes[i];
            best_len = len;
        }
    }
    return best;
}

/* ───────────────────────── 백엔드에 연결 ───────────────────────── */

// 클라이언트 쪽 소켓: getaddrinfo로 주소를 찾고 connect 한다. (CSAPP open_clientfd)
static int open_clientfd(const char *host, const char *port)
{
    struct addrinfo hints, *list, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;   // TCP
    hints.ai_flags = AI_NUMERICSERV;   // port는 숫자

    int rc = getaddrinfo(host, port, &hints, &list);   // 이름 → 주소 (필요하면 DNS 조회)
    if (rc != 0)
    {
        fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (p = list; p != NULL; p = p->ai_next)   // 주소가 여러 개면 되는 것 하나 찾기
    {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;                                // 연결 성공 (3-way handshake)
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

// Round-Robin: 다음 백엔드부터 차례로 연결을 시도한다. 전부 실패하면 -1
static int connect_backend(struct route *r, struct backend **used)
{
    // 여러 워커가 동시에 next를 올리므로 원자적으로 더한다
    unsigned int start = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);

    for (int i = 0; i < r->nbackends; i++)
    {
        struct backend *b = &r->backends[(start + i) % r->nbackends];
        int fd = open_clientfd(b->host, b->port);
        if (fd >= 0)
        {
            *used = b;
            return fd;
        }
        fprintf(stderr, "  백엔드 %s:%s 연결 실패, 다음 백엔드 시도\n", b->host, b->port);
    }
    return -1;
}

/* ───────────────────────── 요청 전달 + 응답 중계 ───────────────────────── */

// 헤더 중 연결 관리용 줄(Connection, Keep-Alive)은 빼고, "Connection: close"를 붙여 보낸다.
// 백엔드가 응답 후 연결을 닫아 줘야 "응답 끝 = EOF"로 중계를 끝낼 수 있기 때문이다.
static int send_request_header(int backendfd, const char *header, size_t header_len)
{
    char *out = malloc(header_len + 32);
    if (out == NULL)
        return -1;

    size_t o = 0;
    const char *p = header;
    const char *end = header + header_len - 2;   // 마지막 빈 줄 "\r\n" 앞까지

    while (p < end)
    {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL || eol > end)
            break;
        size_t len = (eol - p) + 2;

        int skip = strncasecmp(p, "Connection:", 11) == 0 ||
                   strncasecmp(p, "Keep-Alive:", 11) == 0 ||
                   strncasecmp(p, "Proxy-Connection:", 17) == 0;
        if (!skip)
        {
            memcpy(out + o, p, len);
            o += len;
        }
        p = eol + 2;
    }
    memcpy(out + o, "Connection: close\r\n\r\n", 21);
    o += 21;

    int rc = write_all(backendfd, out, o);
    free(out);
    return rc;
}

void proxy_forward(int clientfd, struct route *r,
                   const char *header, size_t header_len,
                   const char *body_head, size_t body_head_len,
                   long content_length)
{
    // 1. 백엔드에 연결 (여기서 우리 서버는 "클라이언트"가 된다)
    struct backend *b = NULL;
    int backendfd = connect_backend(r, &b);
    if (backendfd < 0)
    {
        send_error(clientfd, 502, "Bad Gateway");   // 살아 있는 백엔드가 없음
        return;
    }
    printf("  → 백엔드 %s:%s\n", b->host, b->port);

    // 백엔드가 응답을 너무 오래 안 주면 read가 -1(EAGAIN)로 돌아오게 한다
    struct timeval tv = {PROXY_TIMEOUT_SEC, 0};
    setsockopt(backendfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 2. 요청 헤더 전달
    if (send_request_header(backendfd, header, header_len) < 0)
    {
        send_error(clientfd, 502, "Bad Gateway");
        close(backendfd);
        return;
    }

    // 3. 본문 전달: 이미 받은 부분 먼저, 남은 부분은 손님에게서 읽는 대로 바로 넘김
    if (body_head_len > 0 && write_all(backendfd, body_head, body_head_len) < 0)
    {
        send_error(clientfd, 502, "Bad Gateway");
        close(backendfd);
        return;
    }
    long left = content_length - (long)body_head_len;
    char buf[8192];
    while (left > 0)
    {
        ssize_t n = read(clientfd, buf, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            close(backendfd);   // 손님이 본문을 다 보내기 전에 끊음
            return;
        }
        if (write_all(backendfd, buf, n) < 0)
        {
            send_error(clientfd, 502, "Bad Gateway");
            close(backendfd);
            return;
        }
        left -= n;
    }

    // 4. 응답 중계: 백엔드가 연결을 닫을 때(EOF)까지 읽은 그대로 손님에게 쓴다
    size_t relayed = 0;
    while (1)
    {
        ssize_t n = read(backendfd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && relayed == 0)
                send_error(clientfd, 504, "Gateway Timeout");   // 응답이 아예 안 옴
            else if (relayed == 0)
                send_error(clientfd, 502, "Bad Gateway");
            break;   // 이미 일부를 보냈다면 상태 코드를 바꿀 수 없으니 그냥 끊는다
        }
        if (n == 0)
            break;   // 백엔드가 응답을 다 보내고 닫음
        if (write_all(clientfd, buf, n) < 0)
            break;   // 손님이 끊음
        relayed += n;
    }

    close(backendfd);
}
