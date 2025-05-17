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


// ==================== 기능 함수 선언 =====================
// ======== 서버부 ========
// ==== CLI ====
void show_userinfo(void);   // CLI: users
void show_roominfo(void);   // CLI: rooms
void server_quit(void);     // CLI: quit
// ==== 메시지 전송 ====
void broadcast_to_all(char *msg);               // 전체 사용자에게 메시지 전송
void broadcast_to_room(int room_no, char *msg); // 특정 대화방 참여자에게 메시지 전송
// ==== 사용자 상태 관리 ====
void resp_users(char *buf);                 // 사용자 목록 문자열로 변환
void disconnect_user(userinfo_t *user);     // 연결 종료 시 처리
// ==== 세션 처리 ====
void *client_process(void *args);                            // pthread 함수
void handle_cmd_server(userinfo_t *user, char *input);        // cmd별 분기 처리

// ======== 클라이언트부 ========
// ==== CLI ====
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

void usage_new(userinfo_t *user);
void usage_start(userinfo_t *user);
void usage_accept(userinfo_t *user);
void usage_enter(userinfo_t *user);
void usage_exit(userinfo_t *user);
void usage_dm(userinfo_t *user);
void usage_help(userinfo_t *user);


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

    for (int i = 0; i < client_num; i++) {
        printf("%02d\t%16s\t%12s\t%02d\n", 
            users[i]->sock,
            users[i]->id,
            users[i]->is_conn ? "connected":"disconnected",
            users[i]->chat_room
            );
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
// 메시지 브로드캐스트 함수
void broadcast_to_all(char *msg) {
    for (int i = 0; i < client_num; i++) {
        if (users[i]->is_conn) {
            send(users[i]->sock, msg, strlen(msg), 0);
        }
    }
}

// 특정 대화방 참여자에게 메시지 브로드캐스트 함수
void broadcast_to_room(int room_no, char *msg) {
    for (int i = 0; i < MAX_CLIENT && rooms[room_no - 1].member[i]; i++) {
        send(rooms[room_no - 1].member[i]->sock, msg, strlen(msg), 0);
    }
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
int cmd_chats(userinfo_t *user, char *args) {
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
int cmd_new(userinfo_t *creator, char *room_name) {
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
int cmd_enter(userinfo_t *user, int room_no) {
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
int cmd_enter_wrapper(userinfo_t *user, char *args) {
    char buf[256];
    if (!args) {
        usage_enter(user);
        send(user->sock, buf, strlen(buf), 0);
        return -1;
    }

    int room_no = atoi(args);
    return cmd_enter(user, room_no);
}

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
    char *msg = "Usage: /exit\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_dm(userinfo_t *user) {
    char *msg = "Usage: /dm <target_user_id> <message>\n";
    send(user->sock, msg, strlen(msg), 0);
}

void usage_help(userinfo_t *user) {
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

