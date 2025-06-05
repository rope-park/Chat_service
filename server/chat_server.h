#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include "db_helper.h"
#include "../common/chat_protocol.h"

// ================== 패킷 헤더 및 구조체 정의 ===================
#define HEADER_SIZE         sizeof(PacketHeader)
#define PORTNUM             9000
#define MAX_CLIENT          100
#define BUFFER_SIZE         2048
#define MAX_ROOM_NAME_LEN   32
#define MAX_ID_LEN          20


// User 구조체
typedef struct User {
    int sock;                           // 소켓 번호
    char id[MAX_ID_LEN];                        // 사용자 ID (닉네임)
    pthread_t thread;                   // 사용자별 스레드
    struct Room *room;                  // 대화방 포인터
    struct User *next;                  // 다음 사용자 포인터
    struct User *prev;                  // 이전 사용자 포인터
    struct User *room_user_next;        // 대화방 내 사용자 포인터
    struct User *room_user_prev;        // 대화방 내 사용자 포인터
    int pending_delete;                 // 계정 삭제 대기 여부
} User;

// Room 구조체
typedef struct Room {
    unsigned int no;                    // 방 고유 번호
    char room_name[MAX_ROOM_NAME_LEN];                 // 방 이름
    time_t created_time;                // 생성된 시간
    User *manager;                      // 방장
    int member_count;                   // 생에 참여중인 멤버 수
    User *members[MAX_CLIENT];          // 방에 참여중인 멤버 목록
    struct Room *next;                  // 다음 방 포인터
    struct Room *prev;                  // 이전 방 포인터
} Room;


// ======== 서버 cmd 테이블 구조체 ========
typedef struct {
    const char          *cmd;                // 명령어 문자열
    void                (*cmd_func)(void);   // 해당 명령어 처리 함수
    const char          *comment;            // 도움말 출력용 설명
} server_cmd_t;
extern server_cmd_t cmd_tbl_server[]; // 서버 명령어 테이블

// ======== 클라이언트 cmd 테이블 구조체 ========
typedef struct {
    const char          *cmd;                           // 명령어 문자열
    void                (*cmd_func)(User *, char *);    // 해당 명령어 처리 함수
    void                (*usage_func)(User *);          // 사용법 안내 함수
    const char          *comment;                       // 도움말 출력용 설명
} client_cmd_t;
extern client_cmd_t cmd_tbl_client[]; // 클라이언트 명령어 테이블   


// ================== 전역 변수 ===================
extern User *g_users;                // 연결된 사용자 목록 (헤드 포인터)
extern Room *g_rooms;                // 생성된 대화방 목록 (헤드 포인터)
extern int  g_server_sock;           // 서버 소켓 디스크립터
extern int  g_epfd;                  // epoll 인스턴스 디스크립터
extern unsigned int g_next_room_no;  // 다음 대화방 고유 번호

// 동기화(Mutex) 사용하여 스레드 상호 배제를 통해 안전하게 처리
extern pthread_mutex_t g_users_mutex; // 사용자 목록 보호용 뮤텍스
extern pthread_mutex_t g_rooms_mutex; // 대화방 목록 보호용 뮤텍스
extern pthread_mutex_t g_db_mutex; // 데이터베이스 접근 보호용 뮤텍스


// ==================== 함수 프로토타입 =====================
// ============ 메시지 전송 함수 ============
void send_usage(User *user, const char *usage);
void send_error(User *user, const char *error_msg);
// ============ 목록 / 대화방 관리 래퍼 ============
void list_add_user(User *user);
void list_remove_user(User *user);
User *find_user_by_sock(int sock);
User *find_user_by_id(const char *id);

void list_add_room(Room *room);
void list_remove_room(Room *room);
Room *find_room(const char *name);
Room *find_room_by_no(unsigned int no);

// db + 메모리 동기화를 한 번에 수행하는 함수
void add_user(User *user);                          // 사용자 추가
void remove_user(User *user);                       // 사용자 제거
void add_room(Room *room);                          // 대화방 추가
void remove_room(Room *room);                       // 대화방 제거
void add_user_to_room(Room *room, User *user);      // 대화방 참여자 추가
void remove_user_from_room(Room *room, User *user); // 대화방 참여자 제거
void destroy_room_if_empty(Room *room);             // 대화방이 비어있으면 제거
// ============ 브로드캐스트 함수 ============
void broadcast_server_message_to_room(Room *room, User *sender, const char *message_text); // 특정 방에 있는 모든 사용자(발신자 제외)에 메시지 전송
// ============ 클라이언트 세션 정리 함수 ============
void cleanup_client_session(User *user);

// ============ 서버 CLI 명령어 ============
void server_user(void);                             // users 명령: 사용자 목록
void server_user_info_wrapper(void);                // user_info 명령 래퍼
void server_room_info_wrapper(void);                // room_info 명령 래퍼
void server_room(void);                             // rooms 명령: 대화방 목록
void server_quit(void);                             // quit 명령: 서버 종료
// ============ 클라이언트 CLI 명령어 ============
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

void cmd_leave(User *user);
void cmd_leave_wrapper(User *user, char *args);

void cmd_delete_account(User *user);
void cmd_delete_account_wrapper(User *user, char *args);

void cmd_delete_message(User *user, char *args);

void cmd_help(User *user);
void cmd_help_wrapper(User *user, char *args);

void cmd_usage(User *user);
void cmd_usage_wrapper(User *user, char *args);

void cmd_error(User *user, const char *msg);
void cmd_server_notice(User *user, const char *msg);

void usage_id(User *user);
void usage_manager(User *user);
void usage_change(User *user);
void usage_kick(User *user);
void usage_create(User *user);
void usage_join(User *user);
void usage_leave(User *user);
void usage_delete_account(User *user);
void usage_delete_message(User *user);
void usage_help(User *user);

#endif // CHAT_SERVER_H