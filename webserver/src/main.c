#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <netinet/tcp.h>

#include "http.h"
#include "sbuf.h"
#include "proxy.h"

#define BACKLOG 1024
#define DEFAULT_THREADS 8
#define SBUF_SIZE 64      // 작업 줄에 쌓아 둘 수 있는 연결 수
#define STOP_SIGNAL (-1)  // 작업 줄에 넣는 "그만" 표시 (fd는 음수가 될 수 없으므로)

static sbuf_t sbuf;          // 메인 스레드와 일꾼 스레드가 함께 쓰는 작업 줄
static const char *g_root;   // 정적 파일 루트 (시작할 때 한 번 정하고 읽기만 함)

// 시그널 핸들러가 켜는 종료 표시 stop_requested는 http.c에 있다 (keep-alive 반복에서도 확인하므로).
// 핸들러 안에서는 printf, malloc 같은 함수를 쓰면 안 되므로 "표시만" 한다.

static void on_stop_signal(int sig)
{
    (void)sig;
    int saved_errno = errno;   // 핸들러가 끼어든 곳의 errno를 망가뜨리지 않게 저장 (CSAPP 8.5.5)
    if (atomic_load(&stop_requested))
        _exit(1);              // 두 번째 Ctrl+C: 정리를 기다리지 않고 바로 끝냄
    atomic_store(&stop_requested, 1);
    errno = saved_errno;
}

// 작업 줄에 기다리는 연결이 있나 (http.c의 적응형 keep-alive가 물어봄)
static int queue_has_waiters(void)
{
    return sbuf_count(&sbuf) > 0;
}

// 일꾼 스레드: 작업 줄에서 연결을 하나씩 꺼내 처리한다. "그만" 표시를 꺼내면 끝난다.
static void *worker(void *arg)
{
    (void)arg;

    while (1)
    {
        int connfd = sbuf_remove(&sbuf);   // 비어 있으면 여기서 잠듦
        if (connfd == STOP_SIGNAL)
            break;                         // 메인이 넣은 종료 표시
        handle_client(connfd, g_root);
        close(connfd);
    }
    return NULL;
}

// socket -> setsockopt -> bind -> listen 까지 해서 LISTEN 소켓의 fd를 돌려준다.
// (practice/server.c에서 직접 짠 흐름을 함수로 묶은 것)
static int open_listenfd(int port)
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
    {
        perror("socket");
        return -1;
    }

    // 서버를 껐다 바로 켤 때 TIME_WAIT 때문에 bind가 실패하지 않게 한다.
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(listenfd);
        return -1;
    }

    if (listen(listenfd, BACKLOG) < 0)
    {
        perror("listen");
        close(listenfd);
        return -1;
    }

    return listenfd;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5)
    {
        fprintf(stderr, "사용법: %s <port> <root_dir> [threads] [routes.conf]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port < 1024 || port > 65535)
    {
        fprintf(stderr, "포트는 1024 ~ 65535 사이여야 합니다.\n");
        return 1;
    }
    g_root = argv[2];

    int nthreads = (argc >= 4) ? atoi(argv[3]) : DEFAULT_THREADS;
    if (nthreads < 1 || nthreads > 256)
    {
        fprintf(stderr, "스레드 수는 1 ~ 256 사이여야 합니다.\n");
        return 1;
    }

    // 출력이 파일이나 파이프로 갈 때도 로그가 줄 단위로 바로 찍히게 한다.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // 끊긴 연결에 write 하면 SIGPIPE로 서버 전체가 죽는다. 무시하고 write의 -1로 처리한다.
    signal(SIGPIPE, SIG_IGN);

    // SIGINT(Ctrl+C), SIGTERM(kill)을 받으면 바로 죽지 않고 on_stop_signal을 부르게 한다.
    // SA_RESTART를 일부러 빼서, accept()에서 기다리던 메인 스레드가 -1(EINTR)로 깨어나게 한다.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int listenfd = open_listenfd(port);
    if (listenfd < 0)
        return 1;

    // 라우팅 설정 파일이 있으면 읽는다 (Reverse Proxy)
    if (argc == 5 && proxy_load_routes(argv[4]) < 0)
        return 1;

    // 작업 줄을 만들고, 일꾼 스레드를 미리 만들어 둔다 (prethreading)
    if (sbuf_init(&sbuf, SBUF_SIZE) < 0)
    {
        fprintf(stderr, "작업 줄 생성 실패\n");
        return 1;
    }
    others_waiting = queue_has_waiters;   // 적응형 keep-alive 연결

    // 시그널은 프로세스 안의 아무 스레드에게나 갈 수 있다.
    // 메인 스레드만 받도록, 일꾼을 만드는 동안 막아 두면 일꾼은 "막힌 상태"를 물려받는다.
    sigset_t stop_set, old_set;
    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGINT);
    sigaddset(&stop_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_set, &old_set);

    pthread_t tids[256];
    for (int i = 0; i < nthreads; i++)
    {
        if (pthread_create(&tids[i], NULL, worker, NULL) != 0)
        {
            fprintf(stderr, "스레드 생성 실패\n");
            return 1;
        }
    }

    pthread_sigmask(SIG_SETMASK, &old_set, NULL);   // 메인 스레드는 다시 받을 수 있게

    printf("서버 시작: 포트 %d, 루트 %s, 일꾼 스레드 %d개\n", port, g_root, nthreads);

    // 메인 스레드는 연결을 받아서 작업 줄에 넣는 일만 한다. 종료 표시가 켜지면 멈춘다.
    while (!atomic_load(&stop_requested))
    {
        struct sockaddr_in client;
        socklen_t clientlen = sizeof(client);

        int connfd = accept(listenfd, (struct sockaddr *)&client, &clientlen);
        if (connfd < 0)
        {
            if (errno == EINTR)
                continue;          // 시그널에 깨어남 → 반복 조건에서 stop_requested 확인
            perror("accept");
            continue;
        }

        // 손님이 아무것도 안 보내고 버티면 read가 -1(EAGAIN)로 돌아오게 한다
        struct timeval tv = {CLIENT_TIMEOUT_SEC, 0};
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Nagle 알고리즘 끄기: 헤더와 본문을 write 두 번에 나눠 보내는데,
        // keep-alive에서는 두 번째 작은 조각이 상대의 ACK를 기다리느라 약 40ms씩 늦어진다.
        int one = 1;
        setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        printf("연결 수락: %s:%d (connfd = %d)\n", ip, ntohs(client.sin_port), connfd);

        sbuf_insert(&sbuf, connfd);   // 처리와 close는 일꾼 스레드가 한다
    }

    /* ── 안전한 종료 ── */
    printf("\n종료 신호를 받았습니다. 새 연결을 그만 받고, 처리 중인 요청을 마무리합니다...\n");
    close(listenfd);                          // 1. 새 손님 그만 받기

    for (int i = 0; i < nthreads; i++)        // 2. 일꾼 수만큼 "그만" 표시 넣기
        sbuf_insert(&sbuf, STOP_SIGNAL);      //    (줄 서 있던 손님이 먼저 처리된 뒤 꺼내짐)

    for (int i = 0; i < nthreads; i++)        // 3. 일꾼이 모두 끝날 때까지 기다리기
        pthread_join(tids[i], NULL);

    sbuf_deinit(&sbuf);                       // 4. 작업 줄 정리
    printf("서버를 안전하게 종료했습니다.\n");
    return 0;
}
