#ifndef SBUF_H
#define SBUF_H

#include <pthread.h>

// 연결 fd를 담아 두는 "작업 줄" (크기가 정해진 원형 큐)
// 메인 스레드가 넣고(insert), 일꾼 스레드들이 꺼낸다(remove).
typedef struct
{
    int *buf;               // fd를 담는 배열
    int n;                  // 최대 칸 수
    int front;              // 다음에 꺼낼 위치
    int rear;               // 다음에 넣을 위치
    int count;              // 지금 들어 있는 개수

    pthread_mutex_t mutex;      // 한 번에 한 스레드만 큐를 만지게 하는 자물쇠
    pthread_cond_t not_empty;   // "큐에 뭔가 들어왔다" 신호
    pthread_cond_t not_full;    // "큐에 자리가 생겼다" 신호
} sbuf_t;

int  sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);   // 꽉 차 있으면 자리가 날 때까지 기다림
int  sbuf_remove(sbuf_t *sp);             // 비어 있으면 들어올 때까지 기다림
int  sbuf_count(sbuf_t *sp);              // 지금 줄 서 있는 개수

#endif
