// 직접 구현한 최소 TCP 서버 (금·토)
//  - 금: socket → bind → listen → accept → read → write → close
//  - 토: "\r\n\r\n"이 나올 때까지 read를 반복해 요청 헤더 경계 찾기
// (대화에 붙여 준 코드를 모은 것. 직접 가진 최신 파일이 있으면 그쪽을 쓰세요)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BACKLOG 10
#define BUF_SIZE 4096

int main(void)
{
    // socket() -> bind() -> listen()
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);

    if (listenFd < 0)
    {
        perror("socket");
        return -1;
    }

    // 주소 세팅
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return -1;
    }

    if (listen(listenFd, BACKLOG) < 0)
    {
        perror("listen");
        return -1;
    }

    printf("서버 시작 성공\n");

    while (1)
    {
        struct sockaddr_in client;
        socklen_t clientLen = sizeof(client);

        int connFd = accept(listenFd, (struct sockaddr *)&client, &clientLen);
        if (connFd < 0)
        {
            perror("accept");
            continue;
        }

        // 상대 주소 출력
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        printf("연결 수락: %s:%d (connfd = %d)\n", ip, ntohs(client.sin_port), connFd);

        // "\r\n\r\n"(헤더 끝)이 나올 때까지 read를 반복해 이어 붙인다
        char buf[BUF_SIZE];
        size_t total = 0;
        int foundHeaderEnd = 0;

        while (total < sizeof(buf) - 1)
        {
            ssize_t n = read(connFd, buf + total, (sizeof(buf) - 1) - total);
            if (n <= 0)
            {
                if (n < 0)
                    perror("read");
                break;
            }

            total += n;
            buf[total] = '\0';

            if (strstr(buf, "\r\n\r\n") != NULL)
            {
                foundHeaderEnd = 1;
                break;
            }
        }

        if (!foundHeaderEnd)
        {
            close(connFd);
            continue;
        }
        printf("%s", buf);

        const char *res =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 6\r\n"
            "\r\n"
            "Hello\n";

        if (write(connFd, res, strlen(res)) < 0)
        {
            perror("write");
        }

        close(connFd);
    }

    return 0;
}
