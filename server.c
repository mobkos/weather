#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <locale.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>
#include <curl/curl.h>

#define MAX_CLIENTS 100
#define BUF_SIZE 4096
#define NICK_SIZE 32

#define COLOR_RESET    "\033[0m"
#define COLOR_YELLOW   "\033[33m"
#define COLOR_CYAN     "\033[36m"
#define COLOR_GREEN    "\033[32m"
#define COLOR_BLUE     "\033[34m"
#define COLOR_RED      "\033[31m"

#define KMA_NX "58"
#define KMA_NY "125"
#define KMA_SERVICE_KEY "IayxGddnnCOfOV1nAMov7RRISsZrbItoovEHU3zrGw3wV2mWJrLMbbfoKzv4Jn4DZifO6GleJgcFm%2FK%2Bu6fUWg%3D%3D" // 반드시 본인 키로 교체

typedef struct {
    int sockfd;
    char nickname[NICK_SIZE];
} client_info;

client_info clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int server_sfd = -1;
volatile sig_atomic_t server_running = 1;

time_t last_weather_notice = 0;
time_t last_cloudy_notice = 0;

char initial_weather[BUF_SIZE] = "";

size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    char *buf = (char*)userdata;
    size_t curlen = strlen(buf);
    if (curlen + total >= BUF_SIZE - 1) total = BUF_SIZE - 1 - curlen;
    memcpy(buf + curlen, ptr, total);
    buf[curlen + total] = '\0';
    return size * nmemb;
}

void get_kma_date_time(char *date, char *base_time) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(date, 9, "%Y%m%d", tm);
    int h = tm->tm_hour, m = tm->tm_min;
    int base_h[] = {2,5,8,11,14,17,20,23};
    int use = 2;
    for(int i=0;i<8;i++) if(h>base_h[i] || (h==base_h[i] && m>=30)) use=base_h[i];
    sprintf(base_time, "%02d30", use);
}

const char* weather_emoji(const char* sky, const char* pty) {
    if(strcmp(pty,"1")==0) return "🌧️";
    if(strcmp(pty,"2")==0) return "🌧️";
    if(strcmp(pty,"3")==0) return "🌨️";
    if(strcmp(sky,"1")==0) return "☀️";
    if(strcmp(sky,"4")==0) return "☁️";
    if(strcmp(sky,"3")==0) return "⛅";
    return "";
}

void fetch_kma_weather(char *result, size_t maxlen) {
    char base_date[9], base_time[5];
    get_kma_date_time(base_date, base_time);

    int h = atoi(base_time)/100 + 1;
    char fcstTime[5];
    sprintf(fcstTime, "%02d00", h);

    char url[1024];
    snprintf(url, sizeof(url),
        "http://apis.data.go.kr/1360000/VilageFcstInfoService_2.0/getUltraSrtFcst"
        "?serviceKey=%s&numOfRows=60&pageNo=1&dataType=JSON"
        "&base_date=%s&base_time=%s&nx=%s&ny=%s",
        KMA_SERVICE_KEY, base_date, base_time, KMA_NX, KMA_NY);

    char buf[BUF_SIZE] = {0};
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(result, maxlen, COLOR_YELLOW "[서버] 기상청 API 초기화 실패\n" COLOR_RESET);
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(result, maxlen, COLOR_YELLOW "[서버] 기상청 API 요청 실패: %s\n" COLOR_RESET, curl_easy_strerror(res));
        return;
    }

    char t1h[16]="?", sky[16]="?", pty[16]="?";
    char *p = buf;
    int found = 0;
    while ((p = strstr(p, "\"fcstTime\":\"")) != NULL) {
        char timebuf[8] = {0};
        sscanf(p, "\"fcstTime\":\"%7[^\"]", timebuf);
        if (strcmp(timebuf, fcstTime) != 0) { p++; continue; }
        char *t1h_ptr = strstr(p, "\"category\":\"T1H\"");
        if (t1h_ptr) {
            char *val_ptr = strstr(t1h_ptr, "\"fcstValue\":\"");
            if (val_ptr) sscanf(val_ptr, "\"fcstValue\":\"%15[^\"]", t1h);
        }
        char *sky_ptr = strstr(p, "\"category\":\"SKY\"");
        if (sky_ptr) {
            char *val_ptr = strstr(sky_ptr, "\"fcstValue\":\"");
            if (val_ptr) sscanf(val_ptr, "\"fcstValue\":\"%15[^\"]", sky);
        }
        char *pty_ptr = strstr(p, "\"category\":\"PTY\"");
        if (pty_ptr) {
            char *val_ptr = strstr(pty_ptr, "\"fcstValue\":\"");
            if (val_ptr) sscanf(val_ptr, "\"fcstValue\":\"%15[^\"]", pty);
        }
        found = 1;
        break;
    }
    if (!found || strcmp(t1h,"?")==0) {
        snprintf(result, maxlen, COLOR_YELLOW "[서버] 기상청 API에서 해당 시간의 날씨 데이터를 찾을 수 없습니다.\n" COLOR_RESET);
        return;
    }
    char sky_str[16]="";
    if(strcmp(sky,"1")==0) strcpy(sky_str,"맑음");
    else if(strcmp(sky,"3")==0) strcpy(sky_str,"구름많음");
    else if(strcmp(sky,"4")==0) strcpy(sky_str,"흐림");
    else strcpy(sky_str,"-");
    char pty_str[16]="";
    if(strcmp(pty,"0")==0) strcpy(pty_str,"강수없음");
    else if(strcmp(pty,"1")==0) strcpy(pty_str,"비");
    else if(strcmp(pty,"2")==0) strcpy(pty_str,"비/눈");
    else if(strcmp(pty,"3")==0) strcpy(pty_str,"눈");
    else strcpy(pty_str,"-");

    // 온도만 노란색으로 수동 처리
    char temp[512];
    snprintf(temp, sizeof(temp),
        COLOR_YELLOW "📍" COLOR_RESET "강서구 화곡동 %s시 예보: %s%s, %s, 기온 " COLOR_YELLOW "%s" COLOR_RESET "°C\n",
        fcstTime, weather_emoji(sky,pty), sky_str, pty_str, t1h);
    snprintf(result, maxlen, "%s", temp);
}

float parse_temperature(const char *str) {
    float temp;
    if (sscanf(str, "Temperature: %f", &temp) == 1) {
        return temp;
    }
    return -999;
}
int parse_lux(const char *str) {
    int lux;
    if (sscanf(str, "%d lux", &lux) == 1) {
        return lux;
    }
    return -1;
}

void broadcast_with_color(const char *msg, int sender_sock) {
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < client_count; i++) {
        int client_sock = clients[i].sockfd;
        if (client_sock == sender_sock) {
            dprintf(client_sock, COLOR_GREEN "%s" COLOR_RESET, msg); // 본인: 초록
        } else {
            dprintf(client_sock, "%s%s%s", COLOR_RESET, msg, COLOR_RESET); // 남: 흰색(기본)
        }
    }
    pthread_mutex_unlock(&mutex);
}
void broadcast(const char *msg, int sender_sock, const char *color) {
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < client_count; i++) {
        int client_sock = clients[i].sockfd;
        if (client_sock != sender_sock) {
            dprintf(client_sock, "%s%s%s", color, msg, COLOR_RESET);
        }
    }
    pthread_mutex_unlock(&mutex);
}
void broadcast_shutdown() {
    const char *shutdown_msg = COLOR_RED "[서버] 서버가 종료됩니다. 연결을 종료합니다.\n" COLOR_RESET;
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < client_count; i++) {
        send(clients[i].sockfd, shutdown_msg, strlen(shutdown_msg), 0);
        close(clients[i].sockfd);
    }
    pthread_mutex_unlock(&mutex);
}
void remove_client(int sockfd) {
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sockfd == sockfd) {
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&mutex);
}
void *sensor_monitor(void *arg) {
    int fd_bmp = open("/dev/mybmp", O_RDONLY);
    int fd_bh = open("/dev/mybh", O_RDONLY);
    if (fd_bmp < 0 || fd_bh < 0) {
        perror("센서 장치 열기 실패");
        return NULL;
    }
    char temp_buf[BUF_SIZE], light_buf[BUF_SIZE];
    while (server_running) {
        lseek(fd_bmp, 0, SEEK_SET);
        int n = read(fd_bmp, temp_buf, sizeof(temp_buf)-1);
        if (n > 0) temp_buf[n] = '\0';
        lseek(fd_bh, 0, SEEK_SET);
        n = read(fd_bh, light_buf, sizeof(light_buf)-1);
        if (n > 0) light_buf[n] = '\0';
        float temp = parse_temperature(temp_buf);
        int lux = parse_lux(light_buf);
        time_t now = time(NULL);
        if (lux >= 1000 && temp >= 27.0 && (now - last_weather_notice) >= 10) {
            char notice[256];
            snprintf(notice, sizeof(notice),
                COLOR_YELLOW "[공지]" COLOR_RESET " ☀️ 날씨가 맑습니다. (현재 온도: " COLOR_YELLOW "%.1f" COLOR_RESET "°C, 조도: " COLOR_YELLOW "%d" COLOR_RESET " lux)\n", temp, lux);
            broadcast(notice, -1, "");
            last_weather_notice = now;
        }
        if (lux <= 100 && temp <= 26.0 && (now - last_cloudy_notice) >= 10) {
            char notice[256];
            snprintf(notice, sizeof(notice),
                COLOR_YELLOW "[공지]" COLOR_RESET " ☁️ 날이 흐립니다. (현재 온도: " COLOR_YELLOW "%.1f" COLOR_RESET "°C, 조도: " COLOR_YELLOW "%d" COLOR_RESET " lux)\n", temp, lux);
            broadcast(notice, -1, "");
            last_cloudy_notice = now;
        }
        sleep(1);
    }
    close(fd_bmp); close(fd_bh);
    return NULL;
}

void *client_handler(void *arg) {
    client_info *cinfo = (client_info *)arg;
    int sockfd = cinfo->sockfd;
    char buf[BUF_SIZE];
    char msg_with_nick[BUF_SIZE * 2];
    int bytes_recv;
    const char *ask_nick = COLOR_CYAN "사용할 id를 입력하세요: " COLOR_RESET;
    send(sockfd, ask_nick, strlen(ask_nick), 0);
    memset(cinfo->nickname, 0, NICK_SIZE);
    while (1) {
        bytes_recv = recv(sockfd, cinfo->nickname, NICK_SIZE-1, 0);
        if (bytes_recv <= 0) {
            close(sockfd);
            free(cinfo);
            return NULL;
        }
        size_t nicklen = strlen(cinfo->nickname);
        if (nicklen > 0 && cinfo->nickname[nicklen-1] == '\n')
            cinfo->nickname[nicklen-1] = '\0';
        if (strlen(cinfo->nickname) > 0) break;
        send(sockfd, ask_nick, strlen(ask_nick), 0);
    }
    char welcome[BUF_SIZE*2];
    snprintf(welcome, sizeof(welcome),
        COLOR_CYAN "[알림] 당신의 ID는 " COLOR_GREEN "%s" COLOR_CYAN " 입니다. ☀️'" COLOR_YELLOW "웨더" COLOR_CYAN "'에 오신걸 환영합니다.\n" COLOR_RESET,
        cinfo->nickname);
    send(sockfd, welcome, strlen(welcome), 0);

    while (server_running) {
        memset(buf, 0, sizeof(buf));
        bytes_recv = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (bytes_recv <= 0) break;
        buf[bytes_recv] = '\0';
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

        if (buf[0] == '/') {
            if (strcmp(buf, "/weather") == 0) {
                char reply[BUF_SIZE];
                fetch_kma_weather(reply, sizeof(reply));
                if (strstr(reply, "기상청 API 요청 실패") != NULL ||
                    strstr(reply, "날씨 데이터를 찾을 수 없습니다") != NULL) {
                    send(sockfd, initial_weather, strlen(initial_weather), 0);
                } else {
                    send(sockfd, reply, strlen(reply), 0);
                    strncpy(initial_weather, reply, sizeof(initial_weather)-1);
                }
                continue;
            } else if (strcmp(buf, "/temp") == 0) {
                int fd = open("/dev/mybmp", O_RDONLY);
                if (fd >= 0) {
                    char temp_buf[BUF_SIZE];
                    int n = read(fd, temp_buf, sizeof(temp_buf)-1);
                    if (n > 0) {
                        temp_buf[n] = '\0';
                        float temp = parse_temperature(temp_buf);
                        char msg[128];
                        snprintf(msg, sizeof(msg), "[서버] 현재 온도: " COLOR_YELLOW "%.1f" COLOR_RESET "°C\n", temp);
                        send(sockfd, msg, strlen(msg), 0);
                    } else {
                        const char *msg = COLOR_CYAN "[서버]" COLOR_RESET " 온도 센서 읽기 실패\n";
                        send(sockfd, msg, strlen(msg), 0);
                    }
                    close(fd);
                } else {
                    const char *msg = COLOR_CYAN "[서버]" COLOR_RESET " 온도 센서 장치 열기 실패\n";
                    send(sockfd, msg, strlen(msg), 0);
                }
                continue;
            } else if (strcmp(buf, "/lux") == 0) {
                int fd = open("/dev/mybh", O_RDONLY);
                if (fd >= 0) {
                    char light_buf[BUF_SIZE];
                    int n = read(fd, light_buf, sizeof(light_buf)-1);
                    if (n > 0) {
                        light_buf[n] = '\0';
                        int lux = parse_lux(light_buf);
                        char msg[128];
                        snprintf(msg, sizeof(msg), "[서버] 현재 조도: " COLOR_YELLOW "%d" COLOR_RESET " lux\n", lux);
                        send(sockfd, msg, strlen(msg), 0);
                    } else {
                        const char *msg = COLOR_CYAN "[서버]" COLOR_RESET " 조도 센서 읽기 실패\n";
                        send(sockfd, msg, strlen(msg), 0);
                    }
                    close(fd);
                } else {
                    const char *msg = COLOR_CYAN "[서버]" COLOR_RESET " 조도 센서 장치 열기 실패\n";
                    send(sockfd, msg, strlen(msg), 0);
                }
                continue;
            } else {
                const char *msg = COLOR_CYAN "[서버]" COLOR_RESET " 알 수 없는 명령어입니다. 명령어 목록: /temp, /lux, /weather\n";
                send(sockfd, msg, strlen(msg), 0);
                continue;
            }
        }

        snprintf(msg_with_nick, sizeof(msg_with_nick), "%s: %s\n", cinfo->nickname, buf);
        broadcast_with_color(msg_with_nick, sockfd);
        printf("%s", msg_with_nick);
    }
    printf(COLOR_RED "[서버] %s 클라이언트 연결 종료\n"COLOR_RESET, cinfo->nickname);
    close(sockfd);
    remove_client(sockfd);
    free(cinfo);
    return NULL;
}

void sigint_handler(int sig) {
    (void)sig;
    printf(COLOR_RED "\n[서버] Ctrl+C 신호 감지, 서버 종료 시작...\n" COLOR_RESET);
    server_running = 0;
    if (server_sfd != -1) {
        close(server_sfd);
        server_sfd = -1;
    }
    broadcast_shutdown();
}

int main(void) {
    setlocale(LC_ALL, "");
    struct sockaddr_in server_addr, client_addr;
    socklen_t sock_size;
    int yes = 1;
    pthread_t tid;
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    fetch_kma_weather(initial_weather, sizeof(initial_weather));
    printf(COLOR_CYAN "%s" COLOR_RESET, initial_weather);

    pthread_t sensor_thread;
    pthread_create(&sensor_thread, NULL, sensor_monitor, NULL);

    if ((server_sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket() error");
        exit(1);
    }
    if (setsockopt(server_sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt() error");
        exit(1);
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(10000);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(&(server_addr.sin_zero), '\0', 8);
    if (bind(server_sfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind() error");
        exit(1);
    }
    if (listen(server_sfd, 5) == -1) {
        perror("listen() error");
        exit(1);
    }

    printf(COLOR_CYAN "[서버] " COLOR_YELLOW "채팅 서버 시작!" COLOR_CYAN " 포트: 10000\n" COLOR_RESET);
    printf(COLOR_CYAN "[서버] 채팅 입력 시 모든 클라이언트에게 " COLOR_YELLOW "공지" COLOR_CYAN "로 전송됩니다.\n" COLOR_RESET);
    printf(COLOR_CYAN "[서버] " COLOR_YELLOW "센서 모니터링" COLOR_CYAN " 활성화됨\n" COLOR_RESET);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (server_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_sfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = (server_sfd > STDIN_FILENO) ? server_sfd : STDIN_FILENO;
        struct timeval timeout = {1, 0};

        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0) {
            if (!server_running) break;
            perror("select() error");
            continue;
        }

        if (FD_ISSET(server_sfd, &readfds)) {
            sock_size = sizeof(struct sockaddr_in);
            int client_sfd = accept(server_sfd, (struct sockaddr *)&client_addr, &sock_size);
            if (client_sfd == -1) {
                if (!server_running) break;
                perror("accept() error");
                continue;
            }
            pthread_mutex_lock(&mutex);
            if (client_count >= MAX_CLIENTS) {
                printf("[서버] 최대 클라이언트 수 초과, 접속 거부\n");
                close(client_sfd);
                pthread_mutex_unlock(&mutex);
                continue;
            }
            client_info *cinfo = malloc(sizeof(client_info));
            if (!cinfo) {
                perror("malloc() error");
                close(client_sfd);
                pthread_mutex_unlock(&mutex);
                continue;
            }
            cinfo->sockfd = client_sfd;
            memset(cinfo->nickname, 0, NICK_SIZE);
            clients[client_count++] = *cinfo;
            pthread_mutex_unlock(&mutex);
            printf(COLOR_CYAN "[서버] 새로운 클라이언트 접속: (%s)\n" COLOR_RESET, inet_ntoa(client_addr.sin_addr));
            pthread_create(&tid, NULL, client_handler, (void *)cinfo);
            pthread_detach(tid);
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char input_buf[BUF_SIZE];
            memset(input_buf, 0, sizeof(input_buf));
            if (fgets(input_buf, sizeof(input_buf), stdin) != NULL) {
                size_t len = strlen(input_buf);
                if (len > 0 && input_buf[len - 1] == '\n') {
                    input_buf[len - 1] = '\0';
                }
                if (strlen(input_buf) > 0) {
                    char notice[BUF_SIZE * 2];
                    snprintf(notice, sizeof(notice), COLOR_YELLOW "[공지]" COLOR_RESET "%s\n", input_buf);
                    printf(COLOR_RED "%s" COLOR_RESET, notice);
                    pthread_mutex_lock(&mutex);
                    for (int i = 0; i < client_count; i++) {
                        send(clients[i].sockfd, notice, strlen(notice), 0);
                    }
                    pthread_mutex_unlock(&mutex);
                }
            }
        }
    }

    printf(COLOR_RED "[서버] 메인 루프 종료, 모든 리소스 정리 중...\n" COLOR_RESET);
    server_running = 0;
    pthread_join(sensor_thread, NULL);

    pthread_mutex_lock(&mutex);
    for (int i = 0; i < client_count; i++) {
        close(clients[i].sockfd);
    }
    client_count = 0;
    pthread_mutex_unlock(&mutex);

    if (server_sfd != -1) {
        close(server_sfd);
    }
    curl_global_cleanup();
    printf(COLOR_RED "[서버] 종료 완료\n" COLOR_RESET);
    return 0;
}