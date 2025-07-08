#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <locale.h>

#define BUF_SIZE 512

int sockfd;

void *recv_thread(void *arg) {
    char buf[BUF_SIZE];
    int bytes_recv;

    while (1) {
        memset(buf, 0, sizeof(buf));
        bytes_recv = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (bytes_recv <= 0) {
            printf("[서버 연결 종료]\n");
            exit(0);
        }
        buf[bytes_recv] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, ""); // 한글 지원

    struct sockaddr_in server_addr;
    pthread_t tid;
    char buf[BUF_SIZE];

    if (argc != 2) {
        fprintf(stderr, "사용법: %s <서버 IP>\n", argv[0]);
        exit(1);
    }

    // 소켓 생성
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket() error");
        exit(1);
    }

    // 서버 주소 설정
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(10000);
    server_addr.sin_addr.s_addr = inet_addr(argv[1]);
    memset(&(server_addr.sin_zero), '\0', 8);

    // 서버 연결
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect() error");
        exit(1);
    }

    printf("[클라이언트] 서버에 연결되었습니다. 채팅을 시작하세요!\n");

    // 메시지 수신 스레드 시작
    pthread_create(&tid, NULL, recv_thread, NULL);
    pthread_detach(tid);

    while (1) {
        memset(buf, 0, sizeof(buf));
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        if (strlen(buf) == 0) continue;

        // 한글 깨짐 방지를 위해 strlen이 아닌 바이트 수로 전송
        if (send(sockfd, buf, strlen(buf), 0) == -1) {
            perror("send() error");
            break;
        }
    }

    close(sockfd);
    return 0;
}
