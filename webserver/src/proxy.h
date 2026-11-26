#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

#define MAX_ROUTES 16
#define MAX_BACKENDS 8
#define PROXY_TIMEOUT_SEC 5   // 백엔드가 이 시간 동안 응답이 없으면 504

struct backend
{
    char host[64];
    char port[8];
};

struct route
{
    char prefix[256];                  // 이 경로로 시작하는 요청을
    struct backend backends[MAX_BACKENDS];   // 이 백엔드들에게 보낸다
    int nbackends;
    unsigned int next;                 // Round-Robin 다음 순번 (여러 스레드가 함께 씀)
};

// 설정 파일을 읽어 라우팅 표를 만든다. 성공 0, 실패 -1
int proxy_load_routes(const char *path);

// uri에 맞는 라우트를 찾는다 (가장 긴 prefix). 없으면 NULL
struct route *proxy_match(const char *uri);

// 요청을 백엔드에 전달하고, 응답을 손님에게 그대로 중계한다.
// header: 요청 헤더 전체 ("\r\n\r\n" 포함), header_len 바이트
// body_head: 헤더를 읽을 때 이미 딸려 온 본문, body_head_len 바이트
// content_length: 본문 전체 길이 (없으면 0)
void proxy_forward(int clientfd, struct route *r,
                   const char *header, size_t header_len,
                   const char *body_head, size_t body_head_len,
                   long content_length);

#endif
