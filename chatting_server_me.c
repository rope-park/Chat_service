#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>

#define PORTNUM         9000
#define MAX_CLIENT      100
#define BUFFER_SIZE     1024


typedef struct User User;
typedef struct Room Room;
// ==================== 기능 함수 선언 =====================
// ======== 서버부 ========
// ==== CLI ====
void server_user(void);             // users 명령: 사용자 목록
void server_room(void);             // rooms 명령: 대화방 목록
void server_quit(void);             // quit 명령: 서버 종료
// ==== 메시지 전송 ====
ssize_t safe_send(int sock, const char *msg);
void broadcast_to_room(Room *room, User *sender, const char *format, ...);
void broadcast_to_all(User *sender, const char *format, ...);
// ==== 리스트 및 방 관리 ====
void list_add_client_unlocked(User *user);
void list_remove_client_unlocked(User *user);
User *find_client_by_sock_unlocked(int sock);
User *find_client_by_id_unlocked(const char *id);

void list_add_room_unlocked(Room *room);
void list_remove_room_unlocked(Room *room);
Room *find_room_unlocked(const char *name);
Room *find_room_by_id_unlocked(unsigned int id);

void room_add_member_unlocked(Room *room, User *user);
void room_remove_member_unlocked(Room *room, User *user);
void destroy_room_if_empty_unlocked(Room *room);
// ==== 동기화(Mutex) 래퍼 ====
void list_add_client(User *user);
void list_remove_client(User *user);
User *find_client_by_sock(int sock);
User *find_client_by_id(const char *id);

void list_add_room(Room *room);
void list_remove_room(Room *room);
Room *find_room(const char *name);
Room *find_room_by_id(unsigned int id);

void room_add_member(Room *room, User *user);
void room_remove_member(Room *room, User *user);
void destroy_room_if_empty(Room *room);
// ==== 세션 처리 ====
void *client_process(void *args);
void process_server_cmd(int epfd, int server_sock);

// ======== 클라이언트부 ========
// ==== CLI ====
void cmd_users(User *user);
void cmd_rooms(int sock);
void cmd_id(User *user, const char *args);
void cmd_create(User *creator, const char *room_name);
void cmd_join(User *user, const char *room_no_str);
void cmd_join_wrapper(User *user, char *args);
void cmd_leave(User *user);
void cmd_help(User *user, char *args);

void usage_id(User *user);
void usage_create(User *user);
void usage_join(User *user);
void usage_leave(User *user);
void usage_help(User *user);

// =================== 구조체 ======================
// User 구조체
typedef struct User {
    int sock;                           // 소켓 번호
    char id[64];                        // 사용자 ID (닉네임)
    pthread_t thread;                   // 사용자별 스레드
    struct Room *room;                  // 대화방 포인터
    struct User *next;                  // 다음 사용자 포인터
    struct User *prev;                  // 이전 사용자 포인터
    struct User *room_user_next;        // 대화방 내 사용자 포인터
    struct User *room_user_prev;        // 대화방 내 사용자 포인터
} User;

// Room 구조체
typedef struct Room {
    char room_name[64];                 // 방 이름
    unsigned int no;                    // 방 고유 번호
    struct User *members[MAX_CLIENT];   // 방에 참여중인 멤버 목록
    int member_count;                   // 생에 참여중인 멤버 수
    struct Room *next;                  // 다음 방 포인터
    struct Room *prev;                  // 이전 방 포인터
    int created_time;                   // 생성된 시간
} Room;


// ================== 전역 변수 ===================
static User *g_users = NULL; // 사용자 목록
static Room *g_rooms = NULL; // 대화방 목록
static int g_server_sock = -1; // 서버 소켓
static int g_epfd = -1; // epoll 디스크립터
static unsigned int g_next_room_no = 1; // 다음 대화방 고유 번호

// Mutex 사용하여 스레드 상호 배제를 통해 안전하게 처리
pthread_mutex_t g_users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;


// 서버 cmd 구조체
typedef void (*server_cmd_func_t)(void);
typedef struct {
    const char          *cmd;       // 명령어 문자열
    server_cmd_func_t   cmd_func;   // 해당 명령어 처리 함수
    const char          *comment;   // 도움말 출력용 설명
} server_cmd_t;

// 클라이언트 cmd 구조체
typedef void (*cmd_func_t)(User *user, char *args);
typedef void (*usage_func_t)(User *user);
typedef struct {
    const char          *cmd;            // 명령어 문자열
    cmd_func_t          cmd_func;        // 해당 명령어 처리 함수
    usage_func_t        usage_func;      // 사용법 안내 함수
    const char          *comment;        // 도움말 출력용 설명
} client_cmd_t;


// command 테이블 (서버)
server_cmd_t cmd_tbl_server[] = {
    {"users", server_user, "Show all users"},
    {"rooms", server_room, "Show all chatrooms"},
    {"quit", server_quit, "Quit server"},
    {NULL, NULL, NULL},
};

// command 테이블 (클라이언트)
client_cmd_t cmd_tbl_client[] = {
    {"/users", cmd_users, NULL, "List all users"},
    {"/rooms", cmd_rooms, NULL, "List all chatrooms"},
    {"/id", cmd_id, usage_id, "Change your ID(nickname)"},
    {"/create", cmd_create, usage_create, "Create a new chatroom"},
    {"/join", cmd_join_wrapper, usage_join, "Join a chatroom by number"},
    {"/leave", cmd_leave, usage_leave, "Leave current chatroom"},
    {"/help", cmd_help, usage_help, "Show all available commands"},
    {NULL, NULL, NULL, NULL},
};


// ==================== 기능 함수 구현 ======================
// ======== 서버부 ========
// ==== CLI ====
// 사용자 목록 정보 출력 함수
void server_user(void) {
    pthread_mutex_lock(&g_users_mutex);

    printf("%2s\t%16s\t%20s\n", "SD", "ID", "ROOM");
    printf("=========================================================\n");

    // 사용자 목록을 순회하며 정보 출력
    for (User *u = g_users; u != NULL; u = u->next) {
        // 사용자가 참여 중인 방 이름, 없으면 "Lobby"로 표시
        const char *r = (u->room ? u->room->room_name : "Lobby");
        printf("%2d\t%16s\t%20s\n", 
                u->sock,    // 소켓 번호
                u->id,      // 사용자 ID
                r   // 대화방 이름
        );
    }
    pthread_mutex_unlock(&g_users_mutex);
}

// 생성된 대화방 정보 출력 함수
void server_room(void) {
    pthread_mutex_lock(&g_rooms_mutex);

    printf("%4s\t%20s\t%20s\t%8s\t%s\n", "ID", "ROOM NAME", "CREATED TIME", "#USER", "MEMBER");
    printf("=======================================================================\n");

    // 대화방 목록을 순회하며 정보 출력
    for (Room *r = g_rooms; r != NULL; r = r->next) {
        // 대화방 생성 시간 포맷팅
        char time_str[20];
        struct tm *tm_info = localtime((time_t *)&r->created_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        // 대화방 정보 출력
        printf("%4u\t%20s\t%20s\t%8d\t",
                r->no,               // 방 번호
                r->room_name,        // 방 이름
                time_str,            // 방 생성 시간
                r->member_count      // 방 참여자 수
        );
        for (int i = 0; i < r->member_count; i++) {
            if (r->members[i]) {
                printf("%s", r->members[i]->id);
                if (i < r->member_count - 1) {
                    printf(", ");
                }
            }
        }
        printf("\n");
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

// 서버 종료 함수
void server_quit(void) {
    User *u, *next_u;
    Room *r, *next_r;

    printf("[INFO] Shutting down Server...\n");

    close(g_server_sock); // 서버 소켓 종료

    pthread_mutex_lock(&g_users_mutex);
    // 사용자 목록을 순회하며 연결 종료
    for (u = g_users; u != NULL; u = u->next) {
        if (u->sock >= 0) {
            shutdown(u->sock, SHUT_RDWR);
            close(u->sock);
            u->sock = -1;
        }
    }
    pthread_mutex_unlock(&g_users_mutex);
    
    close(g_epfd); // epoll 디스크립터 종료

    // 사용자 목록 메모리 해제
    pthread_mutex_lock(&g_users_mutex);
    u = g_users;
    while (u) {
        next_u = u->next;
        free(u);
        u = next_u;
    }
    g_users = NULL;
    pthread_mutex_unlock(&g_users_mutex);

    // 대화방 목록 메모리 해제
    pthread_mutex_lock(&g_rooms_mutex);
    r = g_rooms;
    while (r) {
        next_r = r->next;
        free(r);
        r = next_r;
    }
    g_rooms = NULL;
    pthread_mutex_unlock(&g_rooms_mutex);

    printf("[INFO] Server shutdown complete.\n");
    exit(EXIT_SUCCESS);
}

// ==== 메시지 전송 ====
// 안전한 메시지 전송 함수 - 소켓 번호와 메시지 포인터를 인자로 받음
ssize_t safe_send(int sock, const char *msg) {
    if (sock < 0 || msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    ssize_t len = strlen(msg);
    ssize_t total_sent = 0;
    ssize_t bytes_left = len;

    while (bytes_left > 0) {
        ssize_t sent = send(sock, msg + total_sent, bytes_left, 0);
        if (sent < 0) {
            // send() 호출이 인터럽트된 경우 재시도
            if (errno == EINTR) continue;
            perror("send error");
            return -1;
        }
        total_sent += sent;
        bytes_left -= sent;
    }
    return total_sent;
}

// 로비의 모든 사용자에게 메시지 브로드캐스트 함수
void broadcast_to_all(User *sender, const char *format, ...) {
    char msg[BUFFER_SIZE];

    // 메시지 포맷팅 (가변 인자 처리)
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    pthread_mutex_lock(&g_users_mutex);
    User *user = g_users;
    // 사용자 목록을 순회하며 메시지 전송
    while (user != NULL) {
        if (user != sender && user->room == NULL && user->sock >= 0) {
            safe_send(user->sock, msg);
        }
        user = user->next;    
      }
    pthread_mutex_unlock(&g_users_mutex);
}

// 특정 대화방 참여자에게 메시지 브로드캐스트 함수
void broadcast_to_room(Room *room, User *sender, const char *format, ...) {
    if (!room) return; // 대화방이 NULL인 경우 broadcast 하지 않음

    char msg[BUFFER_SIZE];
    // 메시지 포맷팅 (가변 인자 처리)
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    pthread_mutex_lock(&g_rooms_mutex);

    // 대화방 참여자 목록을 순회하며 메시지 전송
    for (int i = 0; i < room->member_count;i++) {
        User *member = room->members[i];
        if (member == NULL || member == sender) continue;
        if (member->sock >= 0) {
            safe_send(member->sock, msg);
        }
        member = member->room_user_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

// ==== 리스트 및 방 관리 ====
// 사용자 추가 함수
void list_add_client_unlocked(User *user) {
    if (g_users == NULL) {
        // 사용자 목록이 비어있는 경우
        g_users = user;
        user->next = NULL;
        user->prev = NULL;
    } else {
        User *current = g_users;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = user;
        user->prev = current;
        user->next = NULL;
    }
}

// 사용자 제거 함수
void list_remove_client_unlocked(User *user) {
    if (user->prev != NULL) {
        user->prev->next = user->next;
    } else {
        g_users = user->next;
    }
    if (user->next != NULL) {
        user->next->prev = user->prev;
    }
    user->prev = NULL;
    user->next = NULL;
}

// 사용자 검색 함수
User *find_client_by_sock_unlocked(int sock) {
    User *current = g_users;
    // 사용자 목록을 순회하며 소켓 번호로 검색
    while (current != NULL) {
        if (current->sock == sock) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 사용자 ID로 검색 함수
User *find_client_by_id_unlocked(const char *id) {
    User *current = g_users;
    // 사용자 목록을 순회하며 ID로 검색
    while (current != NULL) {
        if (strcmp(current->id, id) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 대화방 추가 함수
void list_add_room_unlocked(Room *room) {
    if (g_rooms == NULL) {
        // 대화방 목록이 비어있는 경우
        g_rooms = room;
        room->next = NULL;
        room->prev = NULL;
    } else {
        Room *current = g_rooms;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = room;
        room->prev = current;
        room->next = NULL;
    }
}

// 대화방 제거 함수
void list_remove_room_unlocked(Room *room) {
    if (room->prev != NULL) {
        room->prev->next = room->next;
    } else {
        g_rooms = room->next;
    }
    if (room->next != NULL) {
        room->next->prev = room->prev;
    }
    room->prev = NULL;
    room->next = NULL;
}

// 대화방 검색 함수
Room *find_room_unlocked(const char *name) {
    Room *current = g_rooms;
    // 대화방 목록을 순회하며 이름으로 검색
    while (current != NULL) {
        // 대화방 이름 비교
        if (strcmp(current->room_name, name) == 0) {
            return current;
        }
        current = current->next;
    }
}

// 대화방 ID로 검색 함수
Room *find_room_by_id_unlocked(unsigned int id) {
    Room *current = g_rooms;
    // 대화방 목록을 순회하며 ID로 검색
    while (current != NULL) {
        // 대화방 ID 비교
        if (current->no == id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 대화방 참여자 추가 함수
void room_add_member_unlocked(Room *room, User *user) {
    if (room->members == NULL) {
        // 대화방에 참여자가 없는 경우
        room->members[0] = user;
        user->room_user_next = NULL;
        user->room_user_prev = NULL;
    } else {
        User *current = room->members;
        while (current->room_user_next != NULL) {
            current = current->room_user_next;
        }
        current->room_user_next = user;
        user->room_user_prev = current;
        user->room_user_next = NULL;
    }
    user->room = room; // 사용자 구조체에 대화방 정보 저장
    room->member_count++; // 대화방 참여자 수 증가
}

// 대화방 참여자 제거 함수
void room_remove_member_unlocked(Room *room, User *user) {
    if (user->room_user_prev != NULL) {
        user->room_user_prev->room_user_next = user->room_user_next;
    } else {
        room->members[0] = user->room_user_next;
    }
    if (user->room_user_next != NULL) {
        user->room_user_next->room_user_prev = user->room_user_prev;
    }
    user->room_user_prev = NULL;
    user->room_user_next = NULL;
    user->room = NULL; // 사용자 구조체에 대화방 정보 초기화
    room->member_count--; // 대화방 참여자 수 감소
}

// 대화방이 비어있는 경우 제거 함수
void destroy_room_if_empty_unlocked(Room *room) {
    if (room->member_count <= 0) {
        printf("[INFO] Room '%s' is empty, destroying.\n", room->room_name);
        list_remove_room_unlocked(room);
        free(room);
    }
}


// ==== 동기화(Mutex) 래퍼 ====
// 사용자 추가 함수
void list_add_client(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_add_client_unlocked(user);
    pthread_mutex_unlock(&g_users_mutex);
}

// 사용자 제거 함수
void list_remove_client(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_remove_client_unlocked(user);
    pthread_mutex_unlock(&g_users_mutex);
}

// 사용자 검색 함수
User *find_client_by_sock(int sock) {
    pthread_mutex_lock(&g_users_mutex);
    User *user = find_client_by_sock_unlocked(sock);
    pthread_mutex_unlock(&g_users_mutex);
    return user;
}

// 사용자 ID로 검색 함수
User *find_client_by_id(const char *id) {
    pthread_mutex_lock(&g_users_mutex);
    User *user = find_client_by_id_unlocked(id);
    pthread_mutex_unlock(&g_users_mutex);
    return user;
}

// 대화방 추가 함수
void list_add_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_add_room_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// 대화방 제거 함수
void list_remove_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_remove_room_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// 대화방 검색 함수
Room *find_room(const char *name) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_unlocked(name);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}

// 대화방 ID로 검색 함수
Room *find_room_by_id(unsigned int id) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_by_id_unlocked(id);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}

// 대화방 참여자 추가 함수
void room_add_member(Room *room, User *user) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_add_member_unlocked(room, user);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// 대화방 참여자 제거 함수
void room_remove_member(Room *room, User *user) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_remove_member_unlocked(room, user);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// 대화방이 비어있는 경우 제거 함수
void destroy_room_if_empty(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    destroy_room_if_empty_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// ==== 사용자 상태 관리 ====
// ==== 세션 처리 ====
// pthread 함수
void *client_process(void *args) {
    User *user = (User *)args;
    char buf[BUFFER_SIZE];
    ssize_t bytes_received;

    snprintf(buf, sizeof(buf), "Welcome!\n");
    send(user->sock, buf, strlen(buf), 0);
    while (1) {
        // 초기 ID 요청
        snprintf(buf, sizeof(buf), "Input User ID: ");
        send(user->sock, buf, strlen(buf), 0);
        
        len = recv(user->sock, buf, sizeof(buf) -1, 0);
        if (len <= 0) {
            disconnect_user(user);
            pthread_exit(NULL);
        }
        buf[len] = '\0';

        // 개행 공백 제거
        char *start = buf;
        while (*start && isspace((unsigned char)*start)) start++;
        char *end = start + strlen(start) - 1;
        while (end > start && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }

        // 글자수 오류: 재요청
        if (strlen(start) <= 1 || strlen(start) >= 31) {
            snprintf(buf, sizeof(buf), "ID should be 2 ~ 31 characters. ");
            send(user->sock, buf, strlen(buf), 0);
            continue;
        }

        strncpy(user->id, buf, sizeof(user->id) - 1);
        user->id[sizeof(user->id) - 1] = '\0';
        printf("User [%s] connected\n", user->id);
        snprintf(buf, sizeof(buf), "Connected to Server.\n");
        send(user->sock, buf, strlen(buf), 0);
        break;
    }
    
    while (1) {
        int rb = recv(user->sock, buf, sizeof(buf) - 1, 0);
        if (rb <= 0) {
            // 연결 종료 시 처리
            disconnect_user(user);
            pthread_exit(NULL);
        }

        buf[rb] = '\0';
        strtok(buf, "\n");
        strcpy(buf2, buf);

        // 명령어 처리
        if (buf[0] == '/') {
            char *cmd = strtok(buf2, " ");
            if (!cmd) {
                send(user->sock, "Invalid command.\n", 17, 0);
                continue;
            }

            char *args = strtok(NULL, "");

            int matched = 0;
            for (int i = 0; cmd_tbl_client[i].cmd != NULL; i++) {
                if (strcmp(cmd, cmd_tbl_client[i].cmd) == 0) {
                    matched = 1;
                    cmd_tbl_client[i].cmd_func(user, args);
                    break;
                }
            }

            if (!matched) {
                char *err = "Unknown command. Use /help for command list.\n";
                send(user->sock, err, strlen(err), 0);
            }
        } else {
            // 일반 메시지 처리
            snprintf(buf2, sizeof(buf2), "[%s] %s\n", user->id, buf);
            // 특정 대화방에 메시지 전송
            if (user->chat_room != ROOM0) {
                broadcast_to_room(user->chat_room, buf2);
            } else {
                // 모든 대화방에 메시지 전송
                broadcast_to_all(buf2);
            }
        }
    }
}

// 서버 명령어 처리 함수
void process_server_cmd(int epfd, int server_sock) {
    char cmd_buf[64];

    // 서버 명령어 입력
    if (!fgets(cmd_buf, sizeof(cmd_buf) - 1, stdin)) {
        printf("\n[INFO] Server shutting down... (EOF on stdin).\n");
        server_quit();
        return;
    }

    // 개행 문자 제거
    cmd_buf[strcspn(cmd_buf, "\r\n")] = '\0';

    // 명령어와 인자 분리
    char *cmd = strtok(cmd_buf, " ");

    // 인자 처리 - 명령어가 없거나 빈 문자열인 경우
    if (!cmd || strlen(cmd) == 0) {
        printf("Bad server command.\n");
        fflush(stdout); // 버퍼 비우기
        return;
    }

    // cmd 테이블을 순회하며 명령어 처리
    for (int i = 0; cmd_tbl_server[i].cmd != NULL; i++) {
        if (strcmp(cmd, cmd_tbl_server[i].cmd) == 0) {
            cmd_tbl_server[i].cmd_func();
            printf("> ");
            fflush(stdout);
            return;
        }
    }

    // 명령어가 테이블에 없는 경우
    printf("Unknown server command: %s. Available: users, rooms, quit\n", cmd);
    fflush(stdout);
}


// ======== 클라이언트부 ========
// ==== CLI ====
// 사용자 목록 정보 출력 함수
void cmd_users(User *user) {
    char user_list[BUFFER_SIZE];
    user_list[0] = '\0';

    // 대화방에 참여 중인 경우
    if (user->room) {
        strcat(user_list, "[Server] Users in room ");
        strcat(user_list, user->room->room_name);
        strcat(user_list, ": ");

        pthread_mutex_lock(&g_rooms_mutex);
        User *member = user->room->members;
        // 대화방 참여자 목록을 순회하며 사용자 ID 전송
        while (member != NULL) {
            strcat(user_list, member->id);
            if (member->room_user_next != NULL) {
                strcat(user_list, ", ");
            }
            member = member->room_user_next;
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        strcat(user_list, "\n");
        safe_send(user->sock, user_list);
    } else {
        // 대화방에 참여 중이지 않은 경우
        strcat(user_list, "[Server] Connected users: ");
        pthread_mutex_lock(&g_users_mutex);
        User *user = g_users;
        while (user != NULL) {
            strcat(user_list, user->id);
            if (user->next != NULL) {
                strcat(user_list, ", ");
            }
            user = user->next;
        }
        pthread_mutex_unlock(&g_users_mutex);
        strcat(user_list, "\n");
        safe_send(user->sock, user_list);
    }
}

// 대화방 목록 정보 출력 함수
void cmd_rooms(int sock) {
    char room_list[BUFFER_SIZE];
    room_list[0] = '\0';
    strcat(room_list, "[Server] Available rooms: ");

    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = g_rooms;
    if (room == NULL) {
        strcat(room_list, "No rooms available.\n");
    } else {
        while (room != NULL) {
            int remaining_len = BUFFER_SIZE - strlen(room_list) - 1; // 남은 버퍼 길이
            // 방 정보 포맷팅
            if (remaining_len <= 0) {
                strcat(room_list, "...");
                break;
            }
            int written = snprintf(room_list + strlen(room_list), remaining_len,
                                   "ID %u: '%s' (%d members)", room->no, room->room_name, room->member_count);
            if (written < 0 || written >= remaining_len) {
                strcat(room_list, "...");
                break;
            }

            if (room->next != NULL) {
                remaining_len = BUFFER_SIZE - strlen(room_list) - 1;
                if (remaining_len > strlen(", ")) {
                    strcat(room_list, ", ");
                } else {
                    strcat(room_list, "...");
                    break;
                }
            }
            room = room->next;
        }
        strcat(room_list, "\n");
    }
    pthread_mutex_unlock(&g_rooms_mutex);
    safe_send(sock, room_list);
}

// ID 변경 함수
void cmd_id(User *user, const char *args) {
    // 인자 유효성 검사
    if (!args ||strlen(*args) == 0) {
        usage_id(user);
        return -1;
    }

    char *new_id = strtok(args, " ");
    // ID 길이 제한
    if (new_id == NULL || strlen(new_id) < 2 || strlen(new_id) > 61) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID must be 2 ~ 61 characters long.\n");
        safe_send(user->sock, error_msg);
        return -1;
    }

    // ID 중복 체크
    pthread_mutex_lock(&g_users_mutex);
    User *existing = find_client_by_id_unlocked(new_id);
    pthread_mutex_unlock(&g_users_mutex);

    if (existing && existing != user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID '%s' is already taken.\n", new_id);
        safe_send(user->sock, error_msg);
        return -1;
    }

    // ID 변경
    printf("[INFO] User %s changed ID to %s\n", user->id, new_id);
    strncpy(user->id, new_id, sizeof(user->id) -1);
    user->id[sizeof(user->id) - 1] = '\0';

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] ID updated to %s.\n", user->id);
    safe_send(user->sock, success_msg);
}

// 새 대화방 생성 및 참가 함수
void cmd_create(User *creator, const char *room_name) {
    // 대화방 이름 유효성 검사
    if (!room_name || strlen(room_name) == 0) {
        usage_create(creator);
        return -1;
    }
    
    // 대화방 이름 길이 제한
    if (strlen(room_name) >= sizeof(((Room *)0)->room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name too long (max %zu characters).\n", sizeof(((Room*)0)->room_name) - 1);
        safe_send(creator->sock, error_msg);
        return -1;
    }
    
    // 이미 대화방에 참여 중인지 확인
    if (creator != NULL) {
        safe_send(creator->sock, "[Server] You are already in a room. Please /leave first.\n");
        return -1;
    }

    // 대화방 이름 중복 체크
    pthread_mutex_lock(&g_rooms_mutex);
    Room *existing_room_check = find_room_unlocked(room_name);
    if (existing_room_check != NULL) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(creator->sock, error_msg);
        return -1;
    }

    unsigned int new_room_no = g_next_room_no++;

    // 대화방 구조체 메모리 할당
    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new room failed");
        safe_send(creator->sock, "[Server] Failed to create room.\n");
        return -1;
    }

    strncpy(new_room->room_name, room_name, sizeof(new_room->room_name) - 1);
    new_room->room_name[sizeof(new_room->room_name) - 1] = '\0';
    new_room->no = new_room_no;
    memset(new_room->members, 0, sizeof(new_room->members));
    new_room->member_count = 0;
    new_room->next = NULL;
    new_room->prev = NULL;

    // 대화방 리스트에 추가
    list_add_room_unlocked(new_room);
    pthread_mutex_unlock(&g_rooms_mutex);

    // 대화방에 사용자 추가
    room_add_member(new_room, creator);

    printf("[INFO] User %s created room '%s' (ID: %u) and joined.\n", creator->id, new_room->room_name, new_room->no);
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room '%s' (ID: %u) created and joined.\n", new_room->room_name, new_room->no);
    safe_send(creator->sock, success_msg);
}

// 특정 대화방 참여 함수
void cmd_join(User *user, const char *room_no_str) {
    // 대화방 이름 유효성 검사
    if (!room_no_str || strlen(room_no_str) == 0) {
        usage_join(user);
        return -1;
    }

    // 대화방에 이미 참여 중인지 확인
    if (user->room != NULL) {
        safe_send(user->sock, "[Server] You are already in a room. Please /leave first.\n");
        return -1;
    }

    char *endptr;
    long num_id = strtol(room_no_str, &endptr, 10);

    // 대화방 번호 유효성 검사
    if (*endptr != '\0' || num_id <= 0 || num_id >= g_next_room_no || num_id > 0xFFFFFFFF) {
        safe_send(user->sock, "[Server] Invalid room ID. Please use a valid positive number.\n");
        return -1;
    }

    unsigned int room_no_to_join = (unsigned int)num_id;

    // 대화방 찾기
    Room *target_room = find_room_by_id(room_no_to_join);
    if (!target_room) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room with ID %u not found.\n", room_no_to_join);
        safe_send(user->sock, error_msg);
        return -1;
    }

    room_add_member(target_room, user);
    printf("[INFO] User %s joined room '%s' (ID: %u).\n", user->id, target_room->room_name, target_room->no);
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Joined room '%s' (ID: %u).\n", target_room->room_name, target_room->no);
    safe_send(user->sock, success_msg);
    broadcast_to_room(target_room, user, "[Server] %s joined the room.\n", user->id);
    return 0;
}

// typedef에서 warning: type allocation error 방지
void cmd_join_wrapper(User *user, char *args) {
    char buf[256];
    if (!args) {
        usage_join(user);
        return -1;
    }

    int room_no = atoi(args);
    cmd_join(user, room_no);}

// 현재 대화방 나가기 함수
void cmd_leave(User *user) {
    // 대화방에 미참여한 경우
    if (user->room == NULL) {
        safe_send(user->sock, "[Server] You are not in any room.\n");
        return -1;
    }

    Room *current_room = user->room;
    broadcast_to_room(current_room, user, "[Server] %s left the room.\n", user->id);
    // 대화방에서 사용자 제거
    room_remove_member(current_room, user);
    // 퇴장 메시지 전송
    printf("[INFO] User %s left room '%s' (ID: %u).\n", user->id, current_room->room_name, current_room->no);
    safe_send(user->sock, "[Server] You left the room.\n");
    // 대화방이 비어있으면 제거
    destroy_room_if_empty(current_room);
}

// 도움말 출력 함수
void cmd_help(User *user, char *args) {
    char buf[BUFFER_SIZE];
    int len = 0;

    len = snprintf(buf, sizeof(buf), "Available Commands: \n");

    for (int i = 0; cmd_tbl_client[i].cmd != NULL; i++) {
        // 남은 버퍼 길이 계산
        int rem = sizeof(buf) - len;
        if (rem <= 0) break;

        int written = snprintf(buf + len, rem, "%s\t ----- %s\n", cmd_tbl_client[i].cmd, cmd_tbl_client[i].comment);
        if (written < 0 || written >= rem) {
            snprintf(buf + len, rem, "...\n");
            break;
        }
        len += written; // 누적 길이 업데이트
    }

    safe_send(user->sock, buf);
}


void usage_id(User *user) {
    char *msg = "Usage: /id <new_id(nickname)>\n";
    safe_send(user->sock, msg);
}

void usage_create(User *user) {
    char *msg = "Usage: /create <room_name>\n";
    safe_send(user->sock, msg);
}

void usage_join(User *user) {
    char *msg = "Usage: /join <room_no>\n";
    safe_send(user->sock, msg);
}

void usage_leave(User *user) {
    char *msg = "Usage: /exit\n";
    safe_send(user->sock, msg);
}

void usage_help(User *user) {
    char *msg = "Usage: /help <command>\n";
    safe_send(user->sock, msg);
}


// ================================== 메인 함수 ================================
int main() {
    int sd, ns;
    struct sockaddr_in sin, cli;
    socklen_t clientlen = sizeof(cli);
        
    // 서버 소켓 생성
    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    // SO_REUSEADDR 설정
    int optval = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt");
        exit(1);
    }

    // 서버 주소 설정
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORTNUM);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    // 바인딩
    if (bind(sd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        exit(1);
    }

    // 리스닝
    if (listen(sd, 5) < 0) {
        perror("listen");
        exit(1);
    }

    // epoll 인스턴스 생성 및 이벤트 배열 선언
    int epfd = epoll_create(1);
    struct epoll_event ev, events[MAX_CLIENT];
    int epoll_num = 0;
    
    // 서버 소켓 epoll 등록 (클라이언트 접속 감지용)
    ev.events = EPOLLIN;
    ev.data.fd = sd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sd, &ev);

    // 표준입력 stdin(epoll용) 등록 (관리자 명령 입력)
    ev.events = EPOLLIN;
    ev.data.fd = 0;
    epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);

    printf("Server started.\n");

    // 메인 이벤트 루프
    while (1) {
        // 이벤트 발생한 소켓만 감지
        if ((epoll_num = epoll_wait(epfd, events, MAX_CLIENT, -1)) > 0) {
           for (int i = 0; i < epoll_num; i++) {
                // 서버 소켓: 새 클라이언트 연결 요청 수락
                if (events[i].data.fd == sd) {
                    ns = accept(sd, (struct sockaddr *)&cli, &clientlen);
                    if (client_num >= MAX_CLIENT) {
                        char *msg = "Server is full. Try again later.\n";
                        send(ns, msg, strlen(msg), 0);
                        close(ns);
                    } else {
                        User *user = malloc(sizeof(*user));
                        memset(user, 0, sizeof(*user));
                        user->sock = ns;
                        user->is_conn = 1;
                        user->chat_room = ROOM0;
                        users[client_num++] = user;
                        
                        pthread_create(&user->thread, NULL, client_process, user);
                        pthread_detach(user->thread); // 리소스 자동 회수
                    }
                // stdin 입력 처리 (CLI 명령)
                } else if (events[i].data.fd == 0) {
                    // cmd 처리
                    char input[256];
                    if (fgets(input, sizeof(input), stdin)) {
                        input[strcspn(input, "\n")] = 0;
                        handle_cmd_server(NULL, input);
                    }
                    printf("> ");
                    fflush(stdout);
                }
           }
        }
    }
    close(sd);
    return 0;
}

