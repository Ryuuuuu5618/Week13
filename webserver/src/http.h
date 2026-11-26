#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdatomic.h>

#define REQ_BUF_SIZE 8192
#define CLIENT_TIMEOUT_SEC 10      // 요청을 받는 동안 손님이 아무것도 안 보내면 끊는 시간
#define KEEPALIVE_IDLE_SEC 5       // keep-alive 연결에서 다음 요청을 기다리는 시간
#define MAX_KEEPALIVE_REQUESTS 100 // 연결 하나에서 처리할 최대 요청 수

// 요청 첫 줄 "GET /index.html HTTP/1.1" 을 나눠 담는 구조체
struct request_line
{
    char method[16];    // "GET"
    char uri[1024];     // "/index.html"
    char version[16];   // "HTTP/1.1"
};

// 연결 하나에서 요청을 반복해서 처리한다 (keep-alive). 다 끝나면 연결을 닫는 건 호출한 쪽 몫.
void handle_client(int connfd, const char *root);

// 종료 표시 (main.c의 시그널 핸들러가 켬)
// 메인 스레드의 핸들러가 쓰고 워커 스레드들이 읽으므로, 스레드 사이에서도 안전한 원자 변수로 둔다.
extern atomic_int stop_requested;

// "다른 연결이 줄 서서 기다리는 중인가?"를 알려 주는 함수 (main.c가 작업 큐를 보고 답하도록 등록).
// 기다리는 연결이 있으면 keep-alive를 포기하고 지금 요청까지만 처리해서 워커를 양보한다.
// 등록하지 않으면(NULL, 단위 테스트 등) 항상 keep-alive를 허용한다.
extern int (*others_waiting)(void);

// 아래 함수들은 단위 테스트를 위해 공개해 둔다.
int parse_request_line(const char *buf, struct request_line *rl);
const char *mime_type(const char *path);
long get_content_length(const char *buf);
int get_header(const char *buf, const char *name, char *out, size_t outsize);
int wants_keep_alive(const char *buf, const char *version);

// 프록시(proxy.c)에서도 쓰는 보내기 함수
int write_all(int fd, const void *buf, size_t n);
void send_error(int fd, int status, const char *reason);

#endif
