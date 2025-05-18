// 과제: 대화방 생성/참여/나가기 기능 구현

#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <ctype.h>

#define PORTNUM         2323
#define MAX_CLIENT      100
#define ROOM0           0 
#define MAX_CMD_SIZE    256

<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
// =================== 구조체 ======================
// userinfo 구조체
typedef struct userinfo_t {
    int sock;                       // 소켓 번호
    char id[32];                    // 사용자 ID (닉네임)
    pthread_t thread;               // 사용자별 스레드
    int is_conn;                    // 접속 여부
    int chat_room;                  // 참여중인 대화방 번호
    struct userinfo_t *req_user;    // 1:1 대화 요청 사용자
} userinfo_t;

// roominfo 구조체
typedef struct {
    int no;                         // 방 번호
    char room_name[64];             // 방 이름
    userinfo_t *member[MAX_CLIENT]; // 방에 참여중인 멤버 목록
    int created_time;               // 셍성된 시간
} roominfo_t;

// cmd 구조체
typedef int (*cmd_func_t)(userinfo_t *user, char *args);
typedef void (*usage_func_t)(userinfo_t *user);

// 클라이언트 cmd 구조체
typedef struct {
    char *cmd;                   // 명령어 문자열
    cmd_func_t cmd_func;        // 해당 명령어 처리 함수
    usage_func_t usage_func;    // 사용법 안내 함수
    char *comment;              // 도움말 출력용 설명
} client_cmd_t;

// 서버 cmd 구조체
typedef struct {
    char *cmd;
    void (*cmd_func)(void);
    char *comment;
} server_cmd_t;


// ================== 전역 변수 ===================
static userinfo_t *users[MAX_CLIENT];
static roominfo_t rooms[MAX_CLIENT];
static int client_num = 0;
static int room_num = 0;


=======
typedef struct User User;
typedef struct Room Room;
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
// ==================== 기능 함수 선언 =====================
// ======== 서버부 ========
// ==== CLI ====
void show_userinfo(void);   // CLI: users
void show_roominfo(void);   // CLI: rooms
void server_quit(void);     // CLI: quit
// ==== 메시지 전송 ====
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
void broadcast_to_all(char *msg);               // 전체 사용자에게 메시지 전송
void broadcast_to_room(int room_no, char *msg); // 특정 대화방 참여자에게 메시지 전송
// ==== 사용자 상태 관리 ====
void resp_users(char *buf);                 // 사용자 목록 문자열로 변환
void disconnect_user(userinfo_t *user);     // 연결 종료 시 처리
=======
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
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
// ==== 세션 처리 ====
void *client_process(void *args);                            // pthread 함수
void handle_cmd_server(userinfo_t *user, char *input);        // cmd별 분기 처리

// ======== 클라이언트부 ========
// ==== CLI ====
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
int cmd_users(userinfo_t *user, char *args);
int cmd_chats(userinfo_t *user, char *args);
int cmd_new(userinfo_t *creator, char *room_name);
int cmd_start(userinfo_t *user, char *args);
int cmd_accept(userinfo_t *user, char *args);
int cmd_enter(userinfo_t *user, int room_no);
int cmd_enter_wrapper(userinfo_t *user, char *args);
int cmd_dm(userinfo_t *user, char *args);
int cmd_exit(userinfo_t *user, char *args);
int cmd_help(userinfo_t *user, char *args);
=======
void cmd_users(User *user);
void cmd_rooms(int sock);
void cmd_create(User *creator, const char *room_name);
void cmd_join(User *user, const char *room_no_str);
int cmd_join_wrapper(User *user, char *args);
int cmd_leave(User *user, char *args);
int cmd_help(User *user, char *args);
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c

void usage_new(userinfo_t *user);
void usage_start(userinfo_t *user);
void usage_accept(userinfo_t *user);
void usage_enter(userinfo_t *user);
void usage_exit(userinfo_t *user);
void usage_dm(userinfo_t *user);
void usage_help(userinfo_t *user);

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


<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
=======
// ================== 전역 변수 ===================
static User *g_users = NULL; // 사용자 목록
static Room *g_rooms = NULL; // 대화방 목록
static int g_server_sock = -1; // 서버 소켓
static int g_epdf = -1; // epoll 디스크립터
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


>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
// command 테이블 (서버)
server_cmd_t cmd_tbl_server[] = {
    {"users", show_userinfo, "Show all users"},
    {"chats", show_roominfo, "Show all chatrooms"},
    {"quit", server_quit, "Quit server"},
    {NULL, NULL, NULL},
};

// command 테이블 (클라이언트)
client_cmd_t cmd_tbl_client[] = {
    {"/users", cmd_users, NULL, "List all users"},
    {"/chats", cmd_chats, NULL, "List all chatrooms"},
    {"/new", cmd_new, usage_new, "Create a new chatroom"},
    {"/start", cmd_start, usage_start, "Request for 1:1 chat"},
    {"/accept", cmd_accept, usage_accept, "Accept 1:1 chat request"},
    {"/enter", cmd_enter_wrapper, usage_enter, "Enter a chatroom by number"},
    {"/dm", cmd_dm, usage_dm, "Send message to another users"},
    {"/exit", cmd_exit, usage_exit, "Leave current chatroom"},
    {"/help", cmd_help, usage_help, "Show all available commands"},
    {NULL, NULL, NULL, NULL},
};


// ==================== 기능 함수 구현 ======================
// ======== 서버부 ========
// ==== CLI ====
// 사용자 목록 정보 출력 함수
void show_userinfo(void) {
    printf("%2s\t%16s\t%12s\t%s\n", "SD", "NAME", "CONNECTION", "ROOM");
    printf("=========================================================\n");

<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
    for (int i = 0; i < client_num; i++) {
        printf("%02d\t%16s\t%12s\t%02d\n", 
            users[i]->sock,
            users[i]->id,
            users[i]->is_conn ? "connected":"disconnected",
            users[i]->chat_room
            );
=======
    // 사용자 목록을 순회하며 정보 출력
    for (User *u = g_users; u != NULL; u = u->next) {
        // 사용자가 참여 중인 방 이름, 없으면 "Lobby"로 표시
        const char *r = (u->room ? u->room->room_name : "Lobby");
        printf("%2d\t%16s\t%20s\n", 
                u->sock,    // 소켓 번호
                u->id,      // 사용자 ID
                r   // 대화방 이름
        );
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
    }
}

// 생성된 대화방 정보 출력 함수
void show_roominfo(void) {
    printf("Room\tMembers\n");
    printf("========================================================\n");

    for (int i = 0; i < room_num; i++) {
        printf("%02d\t", rooms[i].no);
        for (int j = 0; j < MAX_CLIENT && rooms[i].member[j]; j++) {
            printf("%s ", rooms[i].member[j]->id);
        }
        printf("\n");
    }
}

// 서버 종료 함수
void server_quit(void) {
    printf("Disconnecting all clients...\n");

    for (int i = 0; i < client_num; i++) {
        if (users[i] && users[i]->is_conn) {
            close(users[i]->sock); // 클라이언트 소켓 종료
            users[i]->is_conn = 0;
            free(users[i]); // 메모리 해제
            users[i] = NULL;
        }
    }
    printf("Server shutdown complete.\n");
    exit(0);
}

// ==== 메시지 전송 ====
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
// 메시지 브로드캐스트 함수
void broadcast_to_all(char *msg) {
    for (int i = 0; i < client_num; i++) {
        if (users[i]->is_conn) {
            send(users[i]->sock, msg, strlen(msg), 0);
=======
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
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
        }
    }
}

// 특정 대화방 참여자에게 메시지 브로드캐스트 함수
void broadcast_to_room(int room_no, char *msg) {
    for (int i = 0; i < MAX_CLIENT && rooms[room_no - 1].member[i]; i++) {
        send(rooms[room_no - 1].member[i]->sock, msg, strlen(msg), 0);
    }
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

}

// 대화방 참여자 추가 함수
void room_add_member_unlocked(Room *room, User *user) {

}

// 대화방 참여자 제거 함수
void room_remove_member_unlocked(Room *room, User *user) {

}

// 대화방이 비어있는 경우 제거 함수
void destroy_room_if_empty_unlocked(Room *room) {

}


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

// ==== 사용자 상태 관리 ====
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
// 클라이언트 목록 문자열로 변환 함수
void resp_users(char *buf) {
    buf[0] = '\0';

    for (int i = 0; i < client_num; i++) {
        strcat(buf, users[i]->id);
        strcat(buf, ", ");
    }
}

// 클라이언트 연결 종료 함수
void disconnect_user(userinfo_t *user) {
    if (!user) return;

    int left_room = user->chat_room;

    // 클라이언트 소켓 종료
    if (user->is_conn) {
        close(user->sock);
        user->is_conn = 0;
        printf("User %s disconnected\n", user->id);
    }

    // 연결 끊긴 사용자 제거
    for (int i = 0; i < client_num; i++) {
        if (users[i] == user) {
            users[i] = users[client_num - 1];
            users[client_num - 1] = NULL;
            client_num--;
            break;
        }
    }

    // 사용자가 참여 중이던 대화방에서 제거
    if (left_room != ROOM0 && left_room <= room_num) {
        roominfo_t *room = &rooms[left_room - 1];
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (room->member[i] == user) {
                room->member[i] == NULL;
                break;
            }
        }

        // 빈 방인지 검사
        int occupied = 0;
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (room->member[i] != NULL) {
                occupied = 1;
                break;
            }
        }

        if (!occupied) {
            // 방 삭제
            for (int i = left_room - 1; i < room_num - 1; i++) {
                rooms[i] = rooms[i + 1];
                rooms[i].no = i + 1;
            }
            room_num--;
            printf("Room %d removed (empty after disconnected)\n", left_room);
        }
    }

    free(user); // 메모리 해제
}

=======
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
// ==== 세션 처리 ====
// pthread 함수
void *client_process(void *args) {
    userinfo_t *user = (userinfo_t *)args;
    char buf[256];
    char buf2[512];
    int len;

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

// cmd별 분기 처리 함수
void handle_cmd_server(userinfo_t *user, char *input) {
    char *cmd = strtok(input, " ");
    char *args = strtok(NULL, ""); // 나머지 인자
    char *msg = "Unknown command.";

    if (!cmd) {
        printf("Bad server command.\n");
        return;
    }

    // 서버 cmd 처리
    for (int i = 0; cmd_tbl_server[i].cmd != NULL; i++) {
        if (strcmp(cmd, cmd_tbl_server[i].cmd) == 0) {
            cmd_tbl_server[i].cmd_func();
            return;
        }
    }

    if (user) {
        send(user->sock, msg, strlen(msg), 0);
    } else {
        printf("%s", msg);
    }
}


// ======== 클라이언트부 ========
// ==== CLI ====
// 사용자 목록 정보 출력 함수
int cmd_users(userinfo_t *user, char *args) {
    char list[256] = {0};
    char msg[512];

    resp_users(list);
    size_t len = strlen(list);
    if (len >= 2) list[len - 2] = '\0';

    snprintf(msg, sizeof(msg),
             "Current User List:\n"
             "%s\n"
             "\n",
             list);

    send(user->sock, msg, strlen(msg), 0);
    return 0;
}

// 대화방 목록 정보 출력 함수
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
int cmd_chats(userinfo_t *user, char *args) {
    char buf[512];
    char line[128];
=======
void cmd_rooms(int sock) {
    char room_list[BUFFER_SIZE];
    room_list[0] = '\0';
    strcat(room_list, "[Server] Available rooms: ");
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c

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

// 새 대화방 생성 및 참가 함수
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
int cmd_new(userinfo_t *creator, char *room_name) {
    char msg[256];
=======
void cmd_create(User *creator, const char *room_name) {
    // 대화방 이름 유효성 검사
    if (!room_name || strlen(room_name) == 0) {
        usage_create(creator);
        return -1;
    }
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
    
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

// 1:1 대화 요청 함수
int cmd_start(userinfo_t *user, char *args) {
    char buf[512];

    if (!args) {
        usage_start(user);
        return -1;
    }

    char buf2[512];
    strncpy(buf2, args, sizeof(buf2) - 1);
    buf2[sizeof(buf2) - 1] = '\0';

    // 인자 분리
    char *id2 = strtok(buf2, " ");
    if (!id2) {
        usage_start(user);
        return -1;
    }

    // 대상 검색
    for (int i = 0; i < client_num; i++) {
        if (strcmp(users[i]->id, id2) == 0 && users[i]->is_conn) {
            users[i]->req_user = user;
            snprintf(buf, sizeof(buf), "[%s] requests you for 1:1 chat. If you want to chat, type /accept.\n", user->id);
            send(users[i]->sock, buf, strlen(buf), 0);
           return 0;
        }
    }

    snprintf(buf, sizeof(buf), "User not found.\n");
    send(user->sock, buf, strlen(buf), 0);
    return -1;
}

// 대화 요청 수락 함수
int cmd_accept(userinfo_t *user, char *args) {
    char buf[256];

    // 수락 가능한 요청 없음
    if (!user->req_user) {
        snprintf(buf, sizeof(buf), "No request for acceptance.\n");
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    userinfo_t *other = user->req_user;
    if (!other->is_conn) {
        snprintf(buf, sizeof(buf), "Request user disconnected.\n");
        send(user->sock, buf, strlen(buf), 0);
        user->req_user = NULL;
        return -1;
    }

    // 새 1:1 대화방 생성
    rooms[room_num].no = room_num + 1;
    strncpy(rooms[room_num].room_name, "1:1 chatroom", sizeof(rooms[room_num].room_name));
    rooms[room_num].member[0] = other;
    rooms[room_num].member[1] = user;

    other->chat_room = room_num + 1;
    user->chat_room = room_num + 1;
    room_num++;

    // 생성 알림
    snprintf(buf, sizeof(buf),  "1:1 Chat(%d) Started.\n", other->chat_room);
    send(other->sock, buf, strlen(buf), 0);
    send(user->sock, buf, strlen(buf), 0);

    user->req_user = NULL;
    return 0;
}

// 특정 대화방 참여 함수
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
int cmd_enter(userinfo_t *user, int room_no) {
    char buf[128];
=======
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
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c

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
<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
int cmd_enter_wrapper(userinfo_t *user, char *args) {
=======
int cmd_join_wrapper(User *user, char *args) {
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
    char buf[256];
    if (!args) {
        usage_enter(user);
        return -1;
    }

    int room_no = atoi(args);
    return cmd_join(user, room_no);}

// 1:1 다이렉트 메시지 전송 함수
int cmd_dm(userinfo_t *user, char *args) {
    fprintf(stderr, "[DEBUG] cmd_dm called by %s, raw args=\"%s\"\n", user->id, args);

    // 인자 검사
    if (!args) {
        usage_dm(user);
        return -1;
    }

    // 원본 args 복사
    char buf[512];
    char buf2[512];
    strncpy(buf2, args, sizeof(buf2) - 1);
    buf2[sizeof(buf2) - 1] = '\0';

    // id2, msg 분리
    char *p = buf2;
    char *id2 = strsep(&p, " ");
    char *msg = p;

    if (!id2 || !msg) {
        usage_dm(user);
        return -1;
    }

    snprintf(buf, sizeof(buf), "[DM] [%s] %s\n",
             user->id, msg);
    fprintf(stderr, "[DEBUG] DM payload=\"%s\"\n", buf);

    // 수신자 검색 및 전송
    for (int i = 0; i < client_num; i++) {
        if (strcmp(users[i]->id, id2) == 0 && users[i]->is_conn) {
            int n = send(users[i]->sock, buf, strlen(buf), 0);
            fprintf(stderr, "[DEBUG] send() returned %d\n", n);

            snprintf(buf, sizeof(buf), "Sent to %s: %s\n",
                     id2, msg);
            send(user->sock, buf, strlen(buf), 0);
            return 0;
        }
    }
    
    // 수신자가 없는 경우
    snprintf(buf, sizeof(buf), "User \"%s\" not found. Can't send the message.\n", id2);
    send(user->sock, buf, strlen(buf), 0);
    return -1;
}

// 현재 대화방 나가기 함수
int cmd_exit(userinfo_t *user, char *args) {
    char buf[128];
    int current_room_no = user->chat_room;
    
    // 대화방에 미참여한 경우
    if (current_room_no == ROOM0) {
        snprintf(buf, sizeof(buf), "You are not in a chatroom.\n");
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    // 해당 대화방에서 사용자 삭제
    roominfo_t *room = &rooms[current_room_no - 1];
    int removed = 0;

    for (int i = 0; i < MAX_CLIENT; i++) {
        // 현재 대화방에 참여한 사용자일 경우
        if (room->member[i] == user) {
            room->member[i] = NULL;
            removed = 1;
            break;
        }
    }

    if (!removed) {
        fprintf(stderr, "[Warning] User %s not found in room %d members list\n", user->id, current_room_no);
    }

    // 사용자의 방 정보 초기화
    user->chat_room = ROOM0;

    // 퇴장 메시지 전송
    snprintf(buf, sizeof(buf), "Exited the chatroom %d(%s)\n", current_room_no, room->room_name);
    send(user->sock, buf, strlen(buf), 0);

    // 빈 방인지 검사
    int occupied = 0;
    for (int i = 0; i <MAX_CLIENT; i++) {
        if (room->member[i] != NULL) {
            occupied = 1;
            break;
        }
    }

    if (!occupied) {
        // 방 삭제
        for (int i = current_room_no - 1; i < room_num - 1; i++) {
            rooms[i] = rooms[i + 1];
            rooms[i].no = i + 1;
        }
        room_num--;
        printf("Room %d removed (empty)\n.", current_room_no);
    }
    return 0;
}

// 도움말 출력 함수
int cmd_help(userinfo_t *user, char *args) {
    char buf[512];
    char line[512];

    snprintf(buf, sizeof(buf), "Available Commands: \n");
    for (int i = 0; cmd_tbl_client[i].cmd != NULL; i++) {
        snprintf(line, sizeof(line), "%s\t ----- %s\n", cmd_tbl_client[i].cmd, cmd_tbl_client[i].comment);
        strcat(buf, line);
    }

    send(user->sock, buf, strlen(buf), 0);
    return 0;
}


<<<<<<< HEAD:chatting_server_cli_pthread_addfunc2.c
void usage_new(userinfo_t *user) {
    char *msg = "Usage: /new <room_name>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_start(userinfo_t *user) {
    char *msg = "Usage: /start <target_user_id>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_accept(userinfo_t *user) {
    char *msg = "Usage: /accept\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_enter(userinfo_t *user) {
    char *msg = "Usage: /enter <room_no>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_exit(userinfo_t *user) {
=======
void usage_create(User *user) {
    char *msg = "Usage: /create <room_name>\n";
    safe_send(user->sock, msg);
}

void usage_join(User *user) {
    char *msg = "Usage: /join <room_no>\n";
    safe_send(user->sock, msg);
}

void usage_leave(User *user) {
>>>>>>> 13b29ab (Feat: list_add_client_unlocked(), list_remove_client_unlocked() 추가):chatting_server_me.c
    char *msg = "Usage: /exit\n";
    safe_send(user->sock, msg);
}

void usage_dm(userinfo_t *user) {
    char *msg = "Usage: /dm <target_user_id> <message>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_help(userinfo_t *user) {
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
                        userinfo_t *user = malloc(sizeof(*user));
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

