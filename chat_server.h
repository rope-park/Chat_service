#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

// 프로토콜 정의
#define REQ_MAGIC 0x5a5a
#define RES_MAGIC 0xa5a5

// 패킷 타입 열거형
typedef enum {
    PACKET_TYPE_MESSAGE = 0,        // C->S: 채팅 메시지, S->C: 브로드캐스트 채팅 메시지 또는 서버 텍스트 메시지
    PACKET_TYPE_SET_USER_ID = 1,   // C->S: 새 사용자 ID 요청, S->C: 결과 / 초기 할당
    PACKET_TYPE_ID_CHANGE = 2,      // C->S: 사용자 ID 변경 요청, S->C: 결과
    PACKET_TYPE_CREATE_ROOM = 3,    // C->S: 요청, S->C: 결과
    PACKET_TYPE_JOIN_ROOM = 4,      // C->S: 요청, S->C: 결과
    PACKET_TYPE_LEAVE_ROOM = 5,     // C->S: 요청, S->C: 결과
    PACKET_TYPE_LIST_ROOMS = 6,     // C->S: 요청, S->C: 대화방 목록 데이터
    PACKET_TYPE_LIST_USERS = 7,     // C->S: 요청 (데이터는 비어있거나 room_id), S->C: 사용자 목록 데이터
    PACKET_TYPE_KICK_USER = 8, // C->S: 대화방에서 사용자 추방 요청, S->C: 결과
    PACKET_TYPE_CHANGE_ROOM_NAME = 9, // C->S: 대화방 이름 변경 요청, S->C: 결과
    PACKET_TYPE_CHANGE_ROOM_MANAGER = 10, // C->S: 대화방 관리자 변경 요청, S->C: 결과
    PACKET_TYPE_DELETE_ACCOUNT = 11, // C->S: 사용자 계정 삭제 요청, S->C: 결과
    PACKET_TYPE_DELETE_MESSAGE = 12, // C->S: 메시지 삭제 요청, S->C: 결과
    PACKET_TYPE_HELP  = 13, // C->S: 도움말 요청, S->C: 도움말 메시지
    PACKET_TYPE_USAGE = 14,
    PACKET_TYPE_ERROR = 15, // C->S: 에러 메시지, S->C: 에러 메시지
    PACKET_TYPE_QUIT = 16, // C->S: 종료 요청, S->C: 종료 메시지
    PACKET_TYPE_SERVER_NOTICE =100, // S->C: 서버 공지 메시지
} PacketType;

#pragma pack(push, 1) // 패딩 없이 구조체 정렬
typedef struct {
    uint16_t magic;        // 패킷 매직 넘버
    uint8_t type;          // 패킷 타입
    uint16_t data_len;     // 데이터 길이
} PacketHeader;

typedef struct {
    uint32_t no;           // 대화방 ID
    char name[32];         // 대화방 이름
    uint16_t member_count; // 멤버 수
} SerializableRoomInfo;
#pragma pack(pop) // 원래 구조체 정렬로 되돌리기


#define HEADER_SIZE     sizeof(PacketHeader)
#define PORTNUM         9000
#define MAX_CLIENT      100
#define BUFFER_SIZE     1024

// User 구조체
typedef struct User {
    int sock;                           // 소켓 번호
    char id[20];                        // 사용자 ID (닉네임)
    int pending_delete;                 // 계정 삭제 대기 여부
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
extern User *g_users;               // 사용자 목록
extern Room *g_rooms;               // 대화방 목록
extern int g_server_sock;           // 서버 소켓
extern int g_epfd;                  // epoll 디스크립터
extern unsigned int g_next_room_no; // 다음 대화방 고유 번호

// Mutex 사용하여 스레드 상호 배제를 통해 안전하게 처리
extern pthread_mutex_t g_users_mutex;
extern pthread_mutex_t g_rooms_mutex;
extern pthread_mutex_t g_db_mutex;


// ==================== 기능 함수 선언 =====================
// ======== 서버부 ========
// ==== CLI ====
void server_user(void);                             // users 명령: 사용자 목록
void server_user_info(char *id);                    // user_info 명령: 사용자 정보 출력
void server_user_info_wrapper(void);                // user_info 명령 래퍼
void server_room_info(char *room_name);             // room_info 명령: 대화방 정보 출력
void server_room_info_wrapper(void);                // room_info 명령 래퍼
void server_room(void);                             // rooms 명령: 대화방 목록
void server_quit(void);                             // quit 명령: 서버 종료
// ==== 메시지 전송 ====
unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length);
ssize_t send_packet(int sock, uint16_t magic, uint8_t type, const void *data, uint16_t data_len);
ssize_t recv_all(int sock, void *buf, size_t len);
ssize_t safe_send(int sock, const char *msg);
void broadcast_to_all(User *sender, const char *format, ...);
void broadcast_to_room(Room *room, User *sender, const char *format, ...);
void broadcast_server_message_to_room(Room *room, User *sender, const char *message_text);
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

int is_room_name_exists(const char *name);
int is_user_id_exists(const char *id);
void room_add_member(Room *room, User *user);
void room_remove_member(Room *room, User *user);
void destroy_room_if_empty(Room *room);

void add_user(User *user);                     // 사용자 추가
void remove_user(User *user);                  // 사용자 제거
void add_room(Room *room);                     // 대화방 추가
void remove_room(Room *room);                  // 대화방 제거
void add_user_to_room(Room *room, User *user); // 대화방 참여자 추가
void remove_user_from_room(Room *room, User *user); // 대화방 참여자 제거

// ==== 세션 처리 ====
void *client_process(void *args);
void process_server_cmd(void);
void parse_and_execute_text_command(User *user, const char *buf);
void cleanup_client_session(User *user);
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
void cmd_delete_message(User *user, char *args);
void cmd_help(User *user);
void cmd_help_wrapper(User *user, char *args);

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