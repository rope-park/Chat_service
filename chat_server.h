#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/types.h>

#define PORTNUM         9000
#define MAX_CLIENT      100
#define BUFFER_SIZE     1024


// User 구조체
typedef struct User {
    int sock;                           // 소켓 번호
    char id[20];                        // 사용자 ID (닉네임)
    pthread_t thread;                   // 사용자별 스레드
    struct Room *room;                  // 대화방 포인터
    struct User *next;                  // 다음 사용자 포인터
    struct User *prev;                  // 이전 사용자 포인터
    struct User *room_user_next;        // 대화방 내 사용자 포인터
    struct User *room_user_prev;        // 대화방 내 사용자 포인터
} User;

// Room 구조체
typedef struct Room {
    char room_name[32];                 // 방 이름
    unsigned int no;                    // 방 고유 번호
    User *manager;                      // 방장
    User *members[MAX_CLIENT];          // 방에 참여중인 멤버 목록
    int member_count;                   // 생에 참여중인 멤버 수
    struct Room *next;                  // 다음 방 포인터
    struct Room *prev;                  // 이전 방 포인터
    time_t created_time;                // 생성된 시간
} Room;

// ================== 전역 변수 ===================
extern User *g_users; // 사용자 목록
extern Room *g_rooms; // 대화방 목록
extern int g_server_sock; // 서버 소켓
extern int g_epfd; // epoll 디스크립터
extern unsigned int g_next_room_no; // 다음 대화방 고유 번호

// Mutex 사용하여 스레드 상호 배제를 통해 안전하게 처리
extern pthread_mutex_t g_users_mutex;
extern pthread_mutex_t g_rooms_mutex;
extern pthread_mutex_t g_db_mutex;


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
void cmd_users_wrapper(User *user, char *args);
void cmd_rooms(int sock);
void cmd_rooms_wrapper(User *user, char *args);
void cmd_id(User *user, char *args);
void cmd_manager(User *user, char *user_id);
void cmd_change(User *user, char *room_name);
void cmd_kick(User *user, char *user_id);
void cmd_create(User *creator, char *room_name);
void cmd_join(User *user, char *room_no_str);
void cmd_join_wrapper(User *user, char *args);
void cmd_leave(User *user);
void cmd_leave_wrapper(User *user, char *args);
void cmd_delete_account(User *user);
void cmd_delete_account_wrapper(User *user, char *args);
void cmd_help(User *user);
void cmd_help_wrapper(User *user, char *args);

void usage_id(User *user);
void usage_manager(User *user);
void usage_change(User *user);
void usage_kick(User *user);
void usage_create(User *user);
void usage_join(User *user);
void usage_leave(User *user);
void usage_help(User *user);


// 서버 cmd 구조체
typedef void (*server_cmd_func_t)(void);
typedef struct {
    const char          *cmd;       // 명령어 문자열
    server_cmd_func_t   cmd_func;   // 해당 명령어 처리 함수
    const char          *comment;   // 도움말 출력용 설명
} server_cmd_t;
extern server_cmd_t cmd_tbl_server[]; // 서버 명령어 테이블

// 클라이언트 cmd 구조체
typedef void (*cmd_func_t)(User *user, char *args);
typedef void (*usage_func_t)(User *user);
typedef struct {
    const char          *cmd;            // 명령어 문자열
    cmd_func_t          cmd_func;        // 해당 명령어 처리 함수
    usage_func_t        usage_func;      // 사용법 안내 함수
    const char          *comment;        // 도움말 출력용 설명
} client_cmd_t;
extern client_cmd_t cmd_tbl_client[]; // 클라이언트 명령어 테이블   

#endif // CHAT_SERVER_H