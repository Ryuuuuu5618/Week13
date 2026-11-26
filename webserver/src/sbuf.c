#include <stdlib.h>
#include "sbuf.h"

// CSAPP 12.5.4의 sbuf를 세마포어 대신 뮤텍스 + 조건 변수로 만든 버전.
// (macOS는 이름 없는 세마포어 sem_init을 지원하지 않아서, 어디서나 되는 방식을 썼다)

int sbuf_init(sbuf_t *sp, int n)
{
    sp->buf = calloc(n, sizeof(int));
    if (sp->buf == NULL)
        return -1;
    sp->n = n;
    sp->front = 0;
    sp->rear = 0;
    sp->count = 0;
    pthread_mutex_init(&sp->mutex, NULL);
    pthread_cond_init(&sp->not_empty, NULL);
    pthread_cond_init(&sp->not_full, NULL);
    return 0;
}

void sbuf_deinit(sbuf_t *sp)
{
    pthread_mutex_destroy(&sp->mutex);
    pthread_cond_destroy(&sp->not_empty);
    pthread_cond_destroy(&sp->not_full);
    free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item)
{
    pthread_mutex_lock(&sp->mutex);              // ── 임계 영역 시작 ──

    while (sp->count == sp->n)                   // 꽉 찼으면
        pthread_cond_wait(&sp->not_full, &sp->mutex);   // 자물쇠를 잠시 풀고 잠듦

    sp->buf[sp->rear] = item;
    sp->rear = (sp->rear + 1) % sp->n;           // 끝에 닿으면 0으로 (원형)
    sp->count++;

    pthread_cond_signal(&sp->not_empty);         // 기다리는 일꾼 하나 깨우기
    pthread_mutex_unlock(&sp->mutex);            // ── 임계 영역 끝 ──
}

int sbuf_count(sbuf_t *sp)
{
    pthread_mutex_lock(&sp->mutex);
    int c = sp->count;
    pthread_mutex_unlock(&sp->mutex);
    return c;
}

int sbuf_remove(sbuf_t *sp)
{
    pthread_mutex_lock(&sp->mutex);              // ── 임계 영역 시작 ──

    while (sp->count == 0)                       // 비었으면
        pthread_cond_wait(&sp->not_empty, &sp->mutex);  // 들어올 때까지 잠듦

    int item = sp->buf[sp->front];
    sp->front = (sp->front + 1) % sp->n;
    sp->count--;

    pthread_cond_signal(&sp->not_full);          // 넣으려고 기다리는 쪽 깨우기
    pthread_mutex_unlock(&sp->mutex);            // ── 임계 영역 끝 ──
    return item;
}
