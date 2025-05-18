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
Room *find_room_by_id(unsigned int id);

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
void process_server_command(int epfd, int server_sock);

// ======== 클라이언트부 ========
// ==== CLI ====
void cmd_users(User *user);
void cmd_rooms(int sock);
int cmd_create(User *creator, char *room_name);
int cmd_join(User *user, int room_no);
int cmd_join_wrapper(User *user, char *args);
int cmd_leave(User *user, char *args);
int cmd_help(User *user, char *args);

void usage_create(User *user);
void usage_join(User *user);
void usage_leave(User *user);
void usage_help(User *user);


// ================== 전역 변수 ===================
static User *g_users = NULL; // 사용자 목록
static Room *g_rooms = NULL; // 대화방 목록
static int g_server_sock = -1; // 서버 소켓
static int g_epdf = -1; // epoll 디스크립터
static unsigned int g_next_room_no = 1; // 다음 대화방 고유 번호

// Mutex 사용하여 스레드 상호 배제를 통해 안전하게 처리
pthread_mutex_t g_users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

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
        const char *room_name = (u->room ? u->room->room_name : "Lobby");
        printf("%2d\t%16s\t%20s\n", 
                u->sock,    // 소켓 번호
                u->id,      // 사용자 ID
                room_name   // 대화방 이름
        );
    }
    pthread_mutex_unlock(&g_users_mutex);
}

// 생성된 대화방 정보 출력 함수
void server_room(void) {
    pthread_mutex_lock(&g_rooms_mutex);

    printf("%4s\t%20s\t%8s\t%s\n", "ID", "ROOM NAME", "#USER", "MEMBER");
    printf("=======================================================================\n");

    // 대화방 목록을 순회하며 정보 출력
    for (Room *r = g_rooms; r != NULL; r = r->next) {
        printf("%4u\t%20s\t%8d\t",
                r->no,               // 방 번호
                r->room_name,        // 방 이름
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
    
    close(g_epdf); // epoll 디스크립터 종료

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
// 메시지 브로드캐스트 함수
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
    User *member = room->members;
    // 대화방 참여자 목록을 순회하며 메시지 전송
    while (member != NULL) {
        if (member->sock >= 0) {
            safe_send(member->sock, msg);
        }
        member = member->room_user_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

// ==== 사용자 상태 관리 ====
// 클라이언트 목록 문자열로 변환 함수
void resp_users(char *buf) {
    buf[0] = '\0';

    for (int i = 0; i < client_num; i++) {
        strcat(buf, users[i]->id);
        strcat(buf, ", ");
    }
}

// 클라이언트 연결 종료 함수
void disconnect_user(User *user) {
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

// ==== 세션 처리 ====
// pthread 함수
void *client_process(void *args) {
    User *user = (User *)args;
    char buf[BUFFER_SIZE];

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
void handle_cmd_server(User *user, char *input) {
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
    char buf[512];
    char line[128];

    if (room_num == 0) {
        snprintf(buf, sizeof(buf), "No chatrooms available.\n");
        send(user->sock, buf, strlen(buf), 0);
        return 0;
    }

    printf("List of Chatrooms:\n");
    printf("%s\t%8s\t%8s\t\n", "INDEX", "ROOM NUMBER", "ROOM NAME");
    printf("===================================================\n");

    for (int i = 1; i <= room_num; i++) {
        snprintf(line, sizeof(line), "%d\t%8d\t ----- %8s\t\n", i, rooms[i].no, rooms[i].room_name);
        strcat(buf, line);

        for (int j = 0; j < MAX_CLIENT && rooms[i].member[j]; j++) {
            strcat(buf, rooms[i].member[j]->id);
            strcat(buf, " ");
        }
        strcat(buf, "\n");
    }

    send(user->sock, buf, strlen(buf), 0);
    return 0;
}

// 새 대화방 생성 및 참가 함수
int cmd_new(User *creator, char *room_name) {
    char msg[256];
    
    if (!room_name || strlen(room_name) == 0) { 
        usage_new(creator);
        return -1;
    }

    // 대화방 이름 중복 체크
    for (int i = 0; i < room_num; i++) {
        if (strcmp(rooms[i].room_name, room_name) == 0) {
            snprintf(msg, sizeof(msg), "Duplicated Room name: %s\n", room_name);
            send(creator->sock, msg, strlen(msg), 0);
            return -1;
        }
    }

    // 새 대화방 생성
    if (room_num >= MAX_CLIENT) {
        snprintf(msg, sizeof(msg), "Room limit reached. Cannot create more chatroms.\n");
        send(creator->sock, msg, strlen(msg), 0);
        return -1;
    }

    rooms[room_num].no = room_num + 1;
    strncpy(rooms[room_num].room_name, room_name, sizeof(rooms[room_num].room_name) - 1);
    rooms[room_num].member[0] = creator;
    creator->chat_room = rooms[room_num].no;
    room_num++;

    snprintf(msg, sizeof(msg), "New room [%s] created and joined.\n", room_name);
    send(creator->sock, msg, strlen(msg), 0);
    return 0;
}

// 특정 대화방 참여 함수
int cmd_enter(User *user, int room_no) {
    char buf[128];

    // 대화방 번호 유효성 검사
    if (room_no <= 0 || room_no > room_num) {
        snprintf(buf, sizeof(buf), "Invalid room number.\n");
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    roominfo_t *room = &rooms[room_no - 1];

    // 1:1 대화방인지 확인
    if (strcmp(room->room_name, "1:1 chatroom") == 0) {
        snprintf(buf, sizeof(buf), "This room is a 1:1 chat. You cannot join.\n");
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    // 이미 해당 방에 들어가 있는지 확인
    if (user->chat_room == room_no) {
        snprintf(buf, sizeof(buf), "You are already in this room %s.\n", room->room_name);
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    // 이미 다른 방에 들어가 있는지 확인
    if (user->chat_room != ROOM0) {
        snprintf(buf, sizeof(buf), "If you want to enter another chat, leave your current room %d\n", user->chat_room);
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    // 대화방 참가
    for (int i = 0; i < MAX_CLIENT; i++) {
        if (room->member[i] == NULL) {
            room->member[i] = user;
            user->chat_room = room->no;
            snprintf(buf, sizeof(buf), "Entered Chatroom %s\n", room->room_name);
            send(user->sock, buf, strlen(buf), 0);
            return 0;
        }
    }

    // 대화방 인원 제한
    snprintf(buf, sizeof(buf), "Room is full.\n");
    send(user->sock, buf, strlen(buf), 0);
    return -1;
}

// typedef에서 warning: type allocation error 방지
int cmd_enter_wrapper(User *user, char *args) {
    char buf[256];
    if (!args) {
        usage_enter(user);
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    int room_no = atoi(args);
    return cmd_enter(user, room_no);
}

// 현재 대화방 나가기 함수
int cmd_exit(User *user, char *args) {
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
int cmd_help(User *user, char *args) {
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


void usage_new(User *user) {
    char *msg = "Usage: /new <room_name>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_enter(User *user) {
    char *msg = "Usage: /enter <room_no>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_exit(User *user) {
    char *msg = "Usage: /exit\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_help(User *user) {
    char *msg = "Usage: /help <command>\n";
    send(user->sock, msg, strlen(msg), 0);
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

