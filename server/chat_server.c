#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sqlite3.h>
#include "chat_server.h"

// ================== 전역 변수 초기화 ===================
User *g_users = NULL; // 사용자 목록
Room *g_rooms = NULL; // 대화방 목록
int g_server_sock = -1; // 서버 소켓
int g_epfd = -1; // epoll 디스크립터
unsigned int g_next_room_no = 1; // 다음 대화방 고유 번호

pthread_mutex_t g_users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_db_mutex    = PTHREAD_MUTEX_INITIALIZER;

// =================== 서버 명령어 테이블 ===================
server_cmd_t cmd_tbl_server[] = {
    {"users",       server_user,              "Show all users"},
    {"user_info",   server_user_info_wrapper, "Show user info by ID"},
    {"room_info",   server_room_info_wrapper, "Show room info by name"},
    {"recent_users", server_user,             "Show recent users"},  // 필요 시 DB 함수 사용
    {"rooms",       server_room,              "Show all chatrooms"},
    {"quit",        server_quit,              "Quit server"},
    {NULL,          NULL,                     NULL}
};

// ================== 클라이언트 명령어 테이블 ===================
client_cmd_t cmd_tbl_client[] = {
    {"/users",            cmd_users_wrapper,        usage_help,           "List all users"},
    {"/rooms",            cmd_rooms_wrapper,        usage_help,           "List all chatrooms"},
    {"/id",               cmd_id,                  usage_id,             "Change your ID"},
    {"/manager",          cmd_manager,             usage_manager,        "Change room manager"},
    {"/change",           cmd_change,               usage_change,         "Change room name"},
    {"/kick",             cmd_kick,                 usage_kick,           "Kick users from the room"},
    {"/create",           cmd_create,               usage_create,         "Create a new chatroom"},
    {"/join",             cmd_join,                 usage_join,           "Join a chatroom by ID"},
    {"/leave",            cmd_leave_wrapper,        usage_leave,          "Leave current chatroom"},
    {"/delete_account",   cmd_delete_account_wrapper, usage_delete_account, "Delete your account"},
    {"/delete_message",   cmd_delete_message,       usage_delete_message, "Delete a message"},
    {"/help",             cmd_help_wrapper,         usage_help,           "Show all available commands"},
    {NULL,                NULL,                     NULL,                  NULL}
};

// ================== 메시지 전송 함수 ===================
// 사용 방법 전송 함수
void send_usage(User *user, const char *usage) {
    if (user && usage) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), " %s\n", usage);
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_USAGE,
            msg,
            (uint16_t)strlen(msg)
        );
    }
}

// 오류 전송 함수
void send_error(User *user, const char *error_msg) {
    if (user && error_msg) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), " Error: %s\n", error_msg);
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_ERROR,
            msg,
            (uint16_t)strlen(msg)
        );
    }
}

// ============ 목록 / 대화방 내부 관리 함수 구현(동기화 미포함 - unlocked 버전) ===========
// 사용자 추가 함수 (전역 사용자 목록에 단순 연결)
void list_add_user_unlocked(User *user) {
    if (g_users == NULL) {
        // 사용자 목록이 비어있는 경우
        g_users = user;
        user->next = NULL;
        user->prev = NULL;
    } else {
        User *current = g_users;
        while (current->next) {
            current = current->next;
        }
        current->next = user;
        user->prev = current;
        user->next = NULL;
    }
}

// 사용자 제거 함수 (전역 사용자 목록에서 단순 연결 해제)
void list_remove_user_unlocked(User *user) {
    if (user == NULL) return;
    if (user->prev) {
        user->prev->next = user->next;
    } else {
        g_users = user->next;
    }
    if (user->next) {
        user->next->prev = user->prev;
    }
    user->prev = NULL;
    user->next = NULL;
}

// 소켓으로 사용자 검색 함수 (전역 사용자 목록에서 단순 검색)
User *find_user_by_sock_unlocked(int sock) {
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

// 사용자 ID로 사용자 검색 함수 (전역 사용자 목록에서 단순 검색)
User *find_user_by_id_unlocked(const char *id) {
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

// 대화방 추가 함수 (전역 대화방 목록에 단순 연결)
void list_add_room_unlocked(Room *room) {
    if (g_rooms == NULL) {
        // 대화방 목록이 비어있는 경우
        g_rooms = room;
        room->next = NULL;
        room->prev = NULL;
    } else {
        Room *current = g_rooms;
        while (current->next) {
            current = current->next;
        }
        current->next = room;
        room->prev = current;
        room->next = NULL;
    }
}

// 대화방 제거 함수 (전역 대화방 목록에서 단순 연결 해제)
void list_remove_room_unlocked(Room *room) {
    if (room == NULL) return;
    if (room->prev) {
        room->prev->next = room->next;
    } else {
        g_rooms = room->next;
    }
    if (room->next) {
        room->next->prev = room->prev;
    }
    room->prev = NULL;
    room->next = NULL;
}

// 대화방 이름으로 대화방 검색 함수 (전역 대화방 목록에서 단순 검색)
Room *find_room_unlocked(const char *name) {
    Room *current = g_rooms;
    while (current != NULL) {
        // 대화방 이름 비교
        if (strcmp(current->room_name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 대화방 번호로 대화방 검색 함수 (전역 대화방 목록에서 단순 검색)
Room *find_room_by_no_unlocked(unsigned int no) {
    Room *current = g_rooms;
    while (current != NULL) {
        // 대화방 번호 비교
        if (current->no == no) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 대화방에 사용자 추가 함수 (배열 + 링크드 리스트 동시 관리)
void room_add_member_unlocked(Room *room, User *user) {
    // 이미 방에 있는지 체크
    for (int i = 0; i < MAX_CLIENT; i++) {
        if (room->members[i] == user) {
            printf("[INFO] User '%s' is already in room '%s'.\n", user->id, room->room_name);
            return;
        }
    }

    // 배열에 빈 슬롯 추가
    int idx = -1;
    for (int i = 0; i < MAX_CLIENT; i++) {
        if (room->members[i] == NULL) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        printf("[ERROR] Room '%s' is full, cannot add member.\n", room->room_name);
        fflush(stdout);
        return; // 대화방이 가득 찬 경우
    }

    // 새 사용자 추가
    room->members[idx] = user;
    room->member_count++; // 대화방 참여자 수 증가

    user->room_user_next = NULL;
    user->room_user_prev = NULL;
    if (room->member_count == 1) {
        // 대화방에 참여자가 없는 경우
        room->members[0] = user;
    } else {
        // 마지막 참여자 찾기
        User *last = NULL;
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (room->members[i] && room->members[i] != user && room->members[i]->room_user_next == NULL) {
                last = room->members[i];
            }
        }
        if (last) {
            last->room_user_next = user;
            user->room_user_prev = last;
        }
    }
    user->room = room; // 사용자 구조체에 대화방 정보 저장
}

// 대화방 사용자 제거 함수 (배열 + 링크드 리스트 동시 관리)
void room_remove_member_unlocked(Room *room, User *user) {
    if (room == NULL || user == NULL) {
        printf("[ERROR] Invalid room or user pointer.\n");
        return;
    }

    // 링크드 리스트 연결 해제
    if (user->room_user_prev) {
        user->room_user_prev->room_user_next = user->room_user_next;
    } else {
        room->members[0] = user->room_user_next;
    }
    if (user->room_user_next) {
        user->room_user_next->room_user_prev = user->room_user_prev;
    }

    // 배열 축소(슬롯 정리)
    int idx = -1;
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == user) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        for (int j = idx; j < room->member_count - 1; j++) {
            room->members[j] = room->members[j + 1];
        }
        room->members[room->member_count - 1] = NULL; // 마지막 요소 NULL로 설정
    }

    user->room_user_prev = NULL;
    user->room_user_next = NULL;
    user->room = NULL; // 사용자 구조체에 대화방 정보 초기화
    room->member_count--; // 대화방 참여자 수 감소
}

// 대화방이 비어있는 경우 제거 함수 (db 동기화 포함)
void destroy_room_if_empty_unlocked(Room *room) {
    if (room->member_count <= 0) {
        printf("[INFO] Room '%s' is empty, destroying.\n", room->room_name);
        fflush(stdout);
        db_remove_room(room); // 데이터베이스에서 대화방 제거
        list_remove_room_unlocked(room);
        free(room);
    }
}

// ================== Mutex(동기화) 래퍼 함수 ==================
// 사용자 추가 함수
void list_add_user(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_add_user_unlocked(user);
    pthread_mutex_unlock(&g_users_mutex);
}

// 사용자 제거 함수
void list_remove_user(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_remove_user_unlocked(user);
    pthread_mutex_unlock(&g_users_mutex);
}

// 사용자 검색 함수
User *find_user_by_sock(int sock) {
    pthread_mutex_lock(&g_users_mutex);
    User *user = find_user_by_sock_unlocked(sock);
    pthread_mutex_unlock(&g_users_mutex);
    return user;
}

// 사용자 ID로 검색 함수
User *find_user_by_id(const char *id) {
    pthread_mutex_lock(&g_users_mutex);
    User *user = find_user_by_id_unlocked(id);
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

// 대화방 번호로 검색 함수
Room *find_room_by_no(unsigned int no) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_by_no_unlocked(no);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}


// 사용자 추가 래퍼 함수
void add_user(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_add_user_unlocked(user); // 사용자 목록에 추가
    pthread_mutex_unlock(&g_users_mutex);

    db_insert_user(user); // 데이터베이스에 사용자 정보 저장
}

// 사용자 제거 래퍼 함수
void remove_user(User *user) {
    pthread_mutex_lock(&g_users_mutex);
    list_remove_user_unlocked(user); // 사용자 목록에서 제거
    pthread_mutex_unlock(&g_users_mutex);

    db_update_user_connected(user, 0); // 데이터베이스에 연결 상태 업데이트
    free(user); // 사용자 구조체 메모리 해제
}

// 대화방 추가 래퍼 함수
void add_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_add_room_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);

    if (!db_create_room(room)) {
        // 데이터베이스에 대화방 생성 실패 시 롤백
        pthread_mutex_lock(&g_rooms_mutex);
        list_remove_room_unlocked(room);
        pthread_mutex_unlock(&g_rooms_mutex);
        fprintf(stderr, "[ERROR] Failed to create room in database.\n");
        free(room);
    }
}

// 대화방 제거 래퍼 함수
void remove_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_remove_room_unlocked(room); // 대화방 목록에서 제거
    pthread_mutex_unlock(&g_rooms_mutex);

    db_remove_room(room); // 데이터베이스에서 대화방 정보 제거
    free(room); // 대화방 구조체 메모리 해제
}

// 대화방 참여자 추가 래퍼 함수
void add_user_to_room(Room *room, User *user) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_add_member_unlocked(room, user);
    pthread_mutex_unlock(&g_rooms_mutex);

    db_add_user_to_room(room, user);
    db_update_room_member_count(room);
}

// 대화방 참여자 제거 래퍼 함수
void remove_user_from_room(Room *room, User *user) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_remove_member_unlocked(room, user); // 대화방 참여자 목록에서 사용자 제거
    pthread_mutex_unlock(&g_rooms_mutex);

    db_remove_user_from_room(room, user); // 데이터베이스에서 사용자 대화방 정보 제거
    db_update_room_member_count(room); // 데이터베이스에 대화방 멤버 수 업데이트

    if (room->member_count == 0) {
        db_remove_room(room); // 대화방이 비어있으면 데이터베이스에서 제거
        pthread_mutex_lock(&g_rooms_mutex);
        list_remove_room_unlocked(room); // 대화방 목록에서 제거
        pthread_mutex_unlock(&g_rooms_mutex);
        free(room); // 대화방 구조체 메모리 해제
    }
}

// 대화방이 비어있는 경우 제거 래퍼 함수
void destroy_room_if_empty(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    destroy_room_if_empty_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

// ============ 브로드캐스트 함수 ============
// 서버 메시지를 대화방 참여자에게 브로드캐스트 함수
void broadcast_server_message_to_room(Room *room, User *sender, const char *message_text) {
    if (!room || !message_text) return; // 대화방이 NULL이거나 메시지가 NULL인 경우

    pthread_mutex_lock(&g_rooms_mutex);
    User *member = room->members[0]; // 대화방 참여자 목록의 첫 번째 사용자
    // 대화방 참여자 목록을 순회하며 서버 메시지 전송
    while (member != NULL) {
        if (member != sender && member->sock >= 0) {
            send_packet(member->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, message_text, (uint16_t)strlen(message_text));
        }
        member = member->room_user_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

// ============ 클라이언트 세션 정리 함수 ============
// 클라이언트 종료 처리(세션 정리) 함수
void cleanup_client_session(User *user) {
    if (!user) return;

    // 1. 방에서 나가기
    if (user->room) {
        Room *room = user->room;
        char disconnect_msg[BUFFER_SIZE];
        snprintf(disconnect_msg, sizeof(disconnect_msg), " %s has disconnected.\n", user->id);

        remove_user_from_room(room, user); // 대화방에서 사용자 제거
        broadcast_server_message_to_room(room, NULL, disconnect_msg); // 대화방 참여자에게 브로드캐스트
        destroy_room_if_empty(room); // 대화방이 비어있으면 제거
    }
    // 2. 사용자 목록에서 제거
    list_remove_user(user);

    // 3. 소켓 종료
    if (user->sock >= 0) {
        shutdown(user->sock, SHUT_RDWR);
        close(user->sock); // 소켓 종료
        user->sock = -1; // 소켓 초기화
    }

    db_update_user_connected(user, 0); // 데이터베이스에 연결 상태 업데이트

    // 사용자 구조체 메모리 해제
    printf("[INFO] Cleaning up client session for user %s (sock=%d).\n", user->id, user->sock);
    free(user); // 사용자 구조체 해제

    pthread_exit(NULL); // 스레드 종료
}

// ==== 서버 명령어 처리 함수 ====
// 서버 사용자 목록 출력 함수
void server_user(void) {
    db_get_all_users(); // 데이터베이스에서 모든 사용자 정보 가져오기
    fflush(stdout); // 출력 버퍼 비우기
}

// 생성된 대화방 정보 출력 함수
void server_room(void) {
    db_get_all_rooms(); // 데이터베이스에서 모든 대화방 정보 가져오기
    fflush(stdout); // 출력 버퍼 비우기
}

// 특정 사용자 정보 출력 함수 - 사용자 ID로 검색
void server_user_info(char *id) {
    if (!id || strlen(id) == 0) {
        printf("user_info <user_id>\n");
        return;
    }

    pthread_mutex_lock(&g_users_mutex);
    find_user_by_id_unlocked(id);
    pthread_mutex_unlock(&g_users_mutex);

    db_get_user_info(id); // 데이터베이스에서 사용자 정보 가져오기
}

// stdin으로 사용자 ID 입력받은 뒤 server_user_info 호출하는 래퍼 함수
void server_user_info_wrapper(void) {
    char id[21];
    printf("Enter user ID to get info: ");
    fflush(stdout);
    if (fgets(id, sizeof(id), stdin) == NULL) {
        perror("fgets error");
        return;
    }
    id[strcspn(id, "\r\n")] = '\0'; // 개행 문자 제거
    server_user_info(id); // 사용자 정보 출력 함수 호출
}

// 대화방 정보 출력 함수 - 대화방 이름으로 검색
void server_room_info(char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        printf("room_info <room_name>\n");
        return;
    }

    pthread_mutex_lock(&g_rooms_mutex);
    Room *r = find_room_unlocked(room_name);
    pthread_mutex_unlock(&g_rooms_mutex);

    if (r) {
        db_get_room_info(r); // 데이터베이스에서 대화방 정보 가져오기
    } else {
        printf("Room '%s' not found.\n", room_name);
    }
}

// stdin으로 대화방 이름 입력받은 뒤 server_room_info 호출하는 래퍼 함수
void server_room_info_wrapper(void) {
    char room_name[33];
    printf("Enter room name to get info: ");
    fflush(stdout);
    if (!fgets(room_name, sizeof(room_name), stdin)) {
        perror("fgets error");
        return;
    }
    room_name[strcspn(room_name, "\r\n")] = '\0'; // 개행 문자 제거
    server_room_info(room_name); // 대화방 정보 출력 함수 호출
}

// 서버 종료 함수 - 서버 종료, 모든 사용자 연결 종료, 메모리 해제, SIGINT 발생
void server_quit(void) {
    User *u, *next_u;
    Room *r, *next_r;

    printf("[INFO] Shutting down Server...\n");

    close(g_server_sock); // 서버 소켓 종료

    // 모든 연결 해제
    pthread_mutex_lock(&g_users_mutex);
    for (u = g_users; u != NULL; u = u->next) {
        if (u->sock >= 0) {
            shutdown(u->sock, SHUT_RDWR);
            close(u->sock);
            u->sock = -1;
        }
    }
    pthread_mutex_unlock(&g_users_mutex);
    
    close(g_epfd); // epoll 디스크립터 종료

    // 사용자 메모리 해제
    pthread_mutex_lock(&g_users_mutex);
    u = g_users;
    while (u) {
        next_u = u->next;
        free(u);
        u = next_u;
    }
    g_users = NULL;
    pthread_mutex_unlock(&g_users_mutex);

    // 대화방 메모리 해제
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
    fflush(stdout);

    raise(SIGINT); // 인터럽트 시그널 발생
}

// ==== 클라이언트 명령어 처리 함수 ====
// 사용자 목록 정보 출력 함수
void cmd_users(User *user) {
    printf("[DEBUG] cmd_users called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    char user_list[BUFFER_SIZE * 2];
    size_t len = 0;
    user_list[0] = '\0';

    if (user->room) {
        len += snprintf(user_list + len, sizeof(user_list) - len, " Users in room %s: ", user->room->room_name);

        pthread_mutex_lock(&g_rooms_mutex);
        User *member = user->room->members[0];
        while (member) {
            size_t rem = sizeof(user_list) - len;
            if (rem <= 1) break;
            len += snprintf(user_list + len, rem, "%s%s", member->id, member->room_user_next ? ", " : "");
            member = member->room_user_next;
        }
        pthread_mutex_unlock(&g_rooms_mutex);
    } else {
        len += snprintf(user_list + len, sizeof(user_list) - len, " Connected users: ");
        pthread_mutex_lock(&g_users_mutex);

        User *iter = g_users;
        while (iter) {
            size_t rem = sizeof(user_list) - len;
            if (rem <= 1) break;
            len += snprintf(user_list + len, rem, "%s%s", iter->id, iter->next ? ", " : "");
            iter = iter->next;
        }
        pthread_mutex_unlock(&g_users_mutex);
    }
    len += snprintf(user_list + len, sizeof(user_list) - len, "\n");

    send_packet(user->sock,
                RES_MAGIC,
                PACKET_TYPE_LIST_USERS,
                user_list,
                (uint16_t)len);

    printf("[INFO] Sent user list to sock=%d\n", user->sock);
    fflush(stdout); // 버퍼 비우기
}

void cmd_users_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_users(user);
}

// 대화방 목록 정보 출력 함수
void cmd_rooms(int sock) {
    char room_list[BUFFER_SIZE * 2];
    size_t len = 0;
    room_list[0] = '\0';

    len += snprintf(room_list + len, sizeof(room_list) - len, " Available rooms: ");

    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = g_rooms;
    if (room == NULL) {
        len += snprintf(room_list + len, sizeof(room_list) - len, "No rooms available.\n");
    } else {
        while (room) {
            size_t rem = sizeof(room_list) - len;
            if (rem < 52) {
                len += snprintf(room_list + len, rem, "...");
                break; // 버퍼가 가득 찬 경우 생략
            }
            // 방 정보 포맷팅
            int written = snprintf(room_list + len, rem,
                "ID %u: '%s' (%d members)%s", 
                room->no,
                room->room_name,
                room->member_count,
                room->next ? ", ": "");
            if (written < 0 || (size_t)written >= rem) {
                len += snprintf(room_list + len, rem, "...");
                break;
            }
            len += (size_t)written;
            room = room->next;
        }
        len += snprintf(room_list + len, sizeof(room_list) - len, "\n");
    }
    pthread_mutex_unlock(&g_rooms_mutex);

    send_packet(
        sock,
        RES_MAGIC,
        PACKET_TYPE_LIST_ROOMS,
        room_list,
        (uint16_t)len
    );

    printf("[INFO] Sent room list to sock=%d\n", sock);
    fflush(stdout); // 버퍼 비우기
}

void cmd_rooms_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_rooms(user->sock);
}

// 사용자 ID 변경 함수
void cmd_id(User *user, char *args) {
    printf("[DEBUG] cmd_id called by %s, sock=%d, args='%s'\n", user->id, user->sock, args ? args : "(null)");
    fflush(stdout); // 버퍼 비우기

    // 인자 유효성 검사
    if (!args ||strlen(args) == 0) {
        usage_id(user);
        return;
    }

    char *new_id = strtok(args, " ");
    // ID 길이 제한
    if (new_id == NULL || strlen(new_id) < 2 || strlen(new_id) > MAX_ID_LEN) {
        char error_msg[] = " ID must be 2 ~ 20 characters long.\n";
        send_error(user, error_msg);
        return;
    }

    // ID 중복 체크
    if (find_user_by_id_unlocked(new_id) || db_check_user_id(new_id)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " ID '%s' already in use. Try another.\n", new_id);
        send_error(user, error_msg);
        return;
    }

    // ID 변경
    db_update_user_id(user, new_id); // 데이터베이스에 사용자 ID 업데이트
    strncpy(user->id, new_id, sizeof(user->id) - 1); // 사용자 구조체에 ID 설정
    user->id[sizeof(user->id) - 1] = '\0';

    char ok[BUFFER_SIZE];
    int n = snprintf(ok, sizeof(ok), " ID changed to '%s'.\n", user->id);
    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_ID_CHANGE,
        ok,
        (uint16_t)n
    );

    printf("[INFO] User %s changed ID to %s (sock=%d)\n", user->id, new_id, user->sock);
    fflush(stdout); // 버퍼 비우기
}

// 방장 변경 함수
void cmd_manager(User *user, char *user_id) {
    printf("[DEBUG] cmd_manager called by %s, sock=%d, user_id='%s'\n", user->id, user->sock, user_id ? user_id : "(null)");
    fflush(stdout); // 버퍼 비우기

    // 사용자가 대화방에 참여 중인지 확인
    if (!user->room) {
        char error_msg[] = " You are not in a room.\n";
        send_error(user, error_msg);
        return;
    }

    Room *r = user->room;
    // 방장 권한 확인
    if (user != r->manager) {
        char error_msg[] = " Only the room manager can change the manager.\n"; 
        send_error(user, error_msg);
        return;
    }

    if (user_id == NULL || strlen(user_id) == 0) {
        usage_manager(user);
        return;
    }

    // 사용자 ID 검색
    User *target_user = find_user_by_id_unlocked(user_id);
    if (!target_user || target_user->room != r) {
        // 사용자 ID가 존재하지 않거나 대화방에 참여 중이지 않은 경우
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " No such User %s in this rooom.\n", user_id);
        send_error(user, error_msg);
        return;
    }
    
    // 본인에게 방장 권한 부여 시도
    if (target_user == user) {
        char error_msg[] = " You are already the manager.\n";
        send_error(user, error_msg);
        return;
    }

    // 방장 변경
    pthread_mutex_lock(&g_rooms_mutex);
    r->manager = target_user;
    pthread_mutex_unlock(&g_rooms_mutex);
    db_update_room_manager(r, target_user->id); // 데이터베이스에 방장 정보 업데이트

    char ok[BUFFER_SIZE];
    snprintf(ok, sizeof(ok), " User '%s' is now the manager of room '%s'.\n", target_user->id, r->room_name);
    broadcast_server_message_to_room(r, NULL, ok); // 방 참여자에게 브로드캐스트

    printf("[INFO] User %s is now the manager of room %s\n", target_user->id, r->room_name);
    fflush(stdout); // 버퍼 비우기
}

// 방 이름 변경 함수
void cmd_change(User *user, char *room_name) {
    printf("[DEBUG] cmd_change called by %s, sock=%d, room_name='%s'\n", user->id, user->sock, room_name ? room_name : "(null)");
    fflush(stdout); // 버퍼 비우기

    // 사용자가 대화방에 참여 중인지 확인
    if (user->room == NULL) {
        char error_msg[] = " You are not in a room.\n";
        send_error(user, error_msg);
        return;
    }

    Room *current = user->room;
    // 방장 권한 확인
    if (current->manager != user) {
        char error_msg[] = " Only the room manager can change the room name.\n";
        send_error(user, error_msg);
        return;
    }
    
    if (room_name == NULL || strlen(room_name) == 0) {
        usage_change(user);
        return;
    }

    // 대화방 이름 중복 검사
    if (find_room_unlocked(room_name) || db_get_room_by_name(room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " Room name '%s' already exists. Please choose a different name.\n", room_name);
        send_error(user, error_msg);
        return;
    }

    // 대화방 이름 변경
    pthread_mutex_lock(&g_rooms_mutex);    
    strncpy(current->room_name, room_name, sizeof(current->room_name) - 1);
    current->room_name[sizeof(current->room_name) - 1] = '\0';
    pthread_mutex_unlock(&g_rooms_mutex);
    
    db_update_room_name(current, current->room_name); // 데이터베이스에 방 이름 업데이트

    char ok[BUFFER_SIZE];
    snprintf(ok, sizeof(ok), " Room name has been changed to '%s'.\n", room_name);
    broadcast_server_message_to_room(current, NULL, ok);

    printf("[INFO] Room name changed to '%s' by user %s (sock=%d)\n", current->room_name, user->id, user->sock);
    fflush(stdout); // 버퍼 비우기
}

// 특정 유저 강제퇴장 함수
void cmd_kick(User *user, char *user_id) {
    printf("[DEBUG] cmd_kick called by %s, sock=%d, user_id='%s'\n", user->id, user->sock, user_id ? user_id : "(null)");
    fflush(stdout); // 버퍼 비우기

    // 사용자가 대화방에 참여 중인지 확인
    if (user->room == NULL) {
        char error_msg[] = " You are not in a room.\n";
        send_error(user, error_msg);
        return;
    }

    Room *current = user->room;
    // 방장 권한 확인
    if (current->manager != user) {
        char error_msg[] = " Only the room manger can kick users.\n";
        send_error(user, error_msg);
        return;
    }

    if (user_id == NULL || strlen(user_id) == 0) {
        usage_kick(user);
        return;
    }
    
    // 사용자 ID 검색
    User *target_user = find_user_by_id_unlocked(user_id);
    // 사용자 존재 여부 확인
    if (target_user == NULL || target_user->room != current) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " User '%s' not found in this room.\n", user_id);
        send_error(user, error_msg);
        return;
    }

    // 본인에게 강퇴 시도
    if (target_user == user) {
        char error_msg[] = " You cannot kick yourself.\n";
        send_error(user, error_msg);
        return;
    }

    remove_user_from_room(current, target_user); // 대화방에서 사용자 제거

    // 강퇴된 사용자에게 메시지 전송
    char kicked_msg[] = " You have been kicked from the room.\n";
    send_packet(
        target_user->sock,
        RES_MAGIC,
        PACKET_TYPE_KICK_USER,
        kicked_msg,
        (uint16_t)strlen(kicked_msg)
    );

    // 방 참여자에게 강퇴 메시지 브로드캐스트
    char kick_msg[BUFFER_SIZE];
    snprintf(kick_msg, sizeof(kick_msg), " User '%s' has been kicked from the room by %s.\n", target_user->id, user->id);
    broadcast_server_message_to_room(current, user, kick_msg); // 방 참여자에게 브로드캐스트

    printf("[INFO] User %s has been kicked from room '%s' by %s.\n", target_user->id, current->room_name, user->id);
    fflush(stdout); // 버퍼 비우기
    
    // 강퇴된 사용자의 세션 정리
    cleanup_client_session(target_user);
}

// 새 대화방 생성 및 참가 함수
void cmd_create(User *creator, char *room_name) {
    printf("[DEBUG] cmd_create called: room_name='%s'\n", room_name ? room_name : "(null)");
    fflush(stdout);

    if (room_name == NULL || strlen(room_name) == 0) {
        usage_create(creator);
        return;
    }

    // 대화방 이름 길이 제한
    if (strlen(room_name) >= sizeof(((Room *)0)->room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " Room name too long (max %zu characters).\n", sizeof(((Room*)0)->room_name) - 1);
        send_error(creator, error_msg);
        return;
    }

    // 대화방 이름 중복 검사
    if (find_room_unlocked(room_name) || db_get_room_by_name(room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " Room name '%s' already exists. Please choose a different name.\n", room_name);
        send_error(creator, error_msg);
        return;
    }

    // 이미 대화방에 참여 중인지 확인
    if (creator->room) {
        char error_msg[] = " You are already in a room. Please /leave first.\n";
        send_error(creator, error_msg);
        return;
    }

    // 방 생성자 ID 유효성 검사
    if (creator == NULL || creator->id[0] == '\0') {
        char error_msg[] = " You must set your ID before creating a room.\n";
        send_error(creator, error_msg);
        return;
    }

    // 대화방 구조체 메모리 할당    
    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new_room failed");
        return;
    }
    memset(new_room->members, 0, sizeof(new_room->members)); // 멤버 초기화
    new_room->no = g_next_room_no++; // 다음 대화방 번호 할당        
    strncpy(new_room->room_name, room_name, sizeof(new_room->room_name) - 1); // 방 이름 설정
    new_room->room_name[sizeof(new_room->room_name) - 1] = '\0';
    new_room->created_time = time(NULL);
    new_room->manager = creator;
    new_room->member_count = 0; // 초기 멤버 수 설정
    new_room->next = new_room->prev = NULL; // 다음 대화방 포인터 초기화
    
    add_room(new_room); // 대화방 목록에 추가

    if (!find_room_by_no_unlocked(new_room->no)) {
        fprintf(stderr, "[ERROR] Failed to add new room %s (ID: %u) to the global room list.\n", new_room->room_name, new_room->no);
        free(new_room); // 메모리 해제
        return;
    }

    add_user_to_room(new_room, creator); // 메모리+DB 동기화
    
    char ok[BUFFER_SIZE];
    int n = snprintf(ok, sizeof(ok), " Room '%s' (ID: %u) created and joined.\n", new_room->room_name, new_room->no);
    send_packet(
        creator->sock,
        RES_MAGIC,
        PACKET_TYPE_CREATE_ROOM,
        ok,
        (uint16_t)n
    );

    printf("[INFO] User %s created room '%s' (ID: %u) and joined.\n", creator->id, new_room->room_name, new_room->no);
    fflush(stdout); // 버퍼 비우기
}

// 특정 대화방 참여 함수
void cmd_join(User *user, char *room_no_str) {
    printf("[DEBUG] cmd_join called: room_no_str='%s'\n", room_no_str ? room_no_str : "(null)");
    fflush(stdout);

    // 대화방 번호 유효성 검사
    if (room_no_str == NULL || strlen(room_no_str) == 0) {
        usage_join(user);
        return;
    }

    // 대화방에 이미 참여 중인지 확인
    if (user->room) {
        char error_msg[] = " You are already in a room. Please /leave first.\n";
        send_error(user, error_msg);
        return;
    }

    unsigned long room_no = strtoul(room_no_str, NULL, 10); // 문자열을 unsigned long으로 변환
    if (room_no == 0) {
        char error_msg[] = " Invalid room ID. Please use a valid positive number.\n";
        send_error(user, error_msg);
        return;
    }

    // 대화방 찾기
    Room *target_room = find_room_by_no_unlocked((unsigned int)room_no);
    if (!target_room) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), " Room with ID %u not found.\n", (unsigned int)room_no);
        send_error(user, error_msg);
        return;
    }

    add_user_to_room(target_room, user); // 메모리+DB 동기화

    char ok[BUFFER_SIZE];
    int n = snprintf(ok, sizeof(ok), " You have joined room '%s' (ID: %u).\n", target_room->room_name, target_room->no);
    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_JOIN_ROOM,
        ok,
        (uint16_t)n
    );
    printf("[INFO] User %s joined room '%s' (ID: %u)\n", user->id, target_room->room_name, target_room->no);
    fflush(stdout); // 버퍼 비우기

    db_get_room_message(target_room, user); // 대화방 메시지 로드

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), " %s has joined the room.\n", user->id);
    broadcast_server_message_to_room(target_room, user, success_msg); // 방 참여자에게 브로드캐스트
}

// 현재 대화방 나가기 함수
void cmd_leave(User *user) {
    printf("[DEBUG] cmd_leave called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기
    
    // 대화방에 미참여한 경우
    if (user->room == NULL) {
        char error_msg[] =" You are not in any room.\n";
        send_error(user, error_msg);
        return;
    }

    Room *current_room = user->room;
    remove_user_from_room(current_room, user); // 메모리+DB 동기화
    
    // 퇴장 메시지 전송
    char ok[] = " You left the room.\n";
    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_LEAVE_ROOM,
        ok,
        (uint16_t)strlen(ok)
    );
    printf("[INFO] User %s has left room '%s' (ID: %u)\n", user->id, current_room->room_name, current_room->no);
    fflush(stdout); // 버퍼 비우기

    char leave_msg[BUFFER_SIZE];
    snprintf(leave_msg, sizeof(leave_msg), " %s has left the room.\n", user->id);
    broadcast_server_message_to_room(current_room, user, leave_msg); // 방 참여자에게 브로드캐스트
}

void cmd_leave_wrapper(User *user, char *args) {
    (void)args;
    cmd_leave(user);
}

// 사용자 계정 삭제 함수
void cmd_delete_account(User *user) {
    printf("[DEBUG] cmd_delete_account called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    // 사용자에게 계정 삭제 확인 메시지 전송
    if (!user->pending_delete) {
        user->pending_delete = 1; // 계정 삭제 요청 플래그로 변경
        const char *confirm_msg = " Are you sure you want to delete your account? Type '/delete_account' again to confirm.\n";
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_DELETE_ACCOUNT,
            confirm_msg,
            (uint16_t)strlen(confirm_msg)
        );
        printf("[INFO] Sending account deletion confirmation to user %s (sock=%d)\n", user->id, user->sock);
        fflush(stdout); // 버퍼 비우기
        return;
    }

    // 실제로 계정 삭제 처리
    user->pending_delete = 0; // 계정 삭제 요청 플래그 초기화
    
    // 사용자 대화방에서 나가기
    if (user->room) {
        cmd_leave(user);
    }

    remove_user(user); // 메모리+DB 동기화

    // 계정 삭제 완료 메시지 전송
    char *msg = " Your account has been deleted.\n";
    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_SERVER_NOTICE,
        msg,
        (uint16_t)strlen(msg)
    );

    // 소켓 종료 및 메모리 해제
    if (user->sock >= 0) {
        shutdown(user->sock, SHUT_RDWR);
        close(user->sock);
    }
    
    printf("[INFO] User %s has deleted their account and disconnected.\n", user->id);
    fflush(stdout); // 버퍼 비우기    
}

void cmd_delete_account_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_delete_account(user);
}

// 메시지 삭제 함수 (방장, 본인만 삭제 가능)
void cmd_delete_message(User *user, char *args) {
    printf("[DEBUG] cmd_delete_message called by %s, sock=%d, args='%s'\n", user->id, user->sock, args ? args : "(null)");
    fflush(stdout); // 버퍼 비우기
    
    if (!args || strlen(args) == 0) {
        usage_delete_message(user);
        return;
    }
    // 인자에서 메시지 ID 추출
    int msg_id = atoi(args);
    if (msg_id <= 0) {
        char error_msg[] = " Invalid message ID.\n";
        send_error(user, error_msg);
        return;
    }

    // 메시지 정보 조회 (sender_id, room_no)
    char sender_id[MAX_ID_LEN] = {0};
    unsigned int room_no = 0;
    pthread_mutex_lock(&g_db_mutex);
    const char *sql = "SELECT sender_id, room_no FROM message WHERE id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, msg_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) { 
            strncpy(sender_id, (const char *)sqlite3_column_text(stmt, 0), sizeof(sender_id) - 1);
            room_no = (unsigned int)sqlite3_column_int(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    if (room_no == 0) {
        char error_msg[] = " Message not found.\n";
        send_error(user, error_msg);
        return;
    }

    Room *room = find_room_by_no_unlocked(room_no);
    if (room == NULL) {
        char error_msg[] = " Room not found.\n";
        send_error(user, error_msg);
        return;
    }

    // 권한 체크: 방장 또는 본인만 삭제 가능
    if (strcmp(user->id, sender_id) != 0 && user != room->manager) {
        char error_msg[] = " Only the sender or the room manager can delete this message.\n";
        send_error(user, error_msg);
        return;
    }

    // 메시지 삭제
    int delete_result = db_remove_message_by_id(room, user, msg_id);
    if (delete_result) {
        char ok[] = " Message deleted successfully.\n";
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_DELETE_MESSAGE,
            ok,
            (uint16_t)strlen(ok)
        );
    } else {
        char error_msg[] = " Failed to delete message.\n";
        send_error(user, error_msg);
    }

    printf("[INFO] User %s deleted message ID %d in room '%s' (ID: %u)\n", user->id, msg_id, room->room_name, room->no);
    fflush(stdout); // 버퍼 비우기
}

// 도움말 출력 함수
void cmd_help(User *user) {
    printf("[DEBUG] cmd_help called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    char buf[BUFFER_SIZE * 2];
    int len = 0;

    len += snprintf(buf + len, sizeof(buf) - 1, "Available Commands: \n");

    for (int i = 0; cmd_tbl_client[i].cmd != NULL; i++) {
        // 남은 버퍼 길이 계산
        size_t rem = sizeof(buf) - len;
        if (rem <= 1) break;
        int written = snprintf(buf + len, rem, "%s\t ----- %s\n", cmd_tbl_client[i].cmd, cmd_tbl_client[i].comment);
        if (written < 0 || (size_t)written >= rem) {
            len += snprintf(buf + len, rem, "...\n");
            break;
        }
        len += (size_t)written; // 누적 길이 업데이트
    }

    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_HELP,
        buf,
        (uint16_t)len
    );

    printf("[INFO] Help message sent to user %s (sock=%d)\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기
}

void cmd_help_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_help(user);
}

// 사용법 출력 함수들
void cmd_usage(User *user) {
    printf("[DEBUG] cmd_usage called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    const char *usage_msg = 
        " Usage:\n"
        "/id <new_id> - Change your ID (nickname)\n"
        "/manager <user_id> - Change room manager\n"
        "/change <room_name> - Change current room name\n"
        "/kick <user_id> - Kick a user from the current room\n"
        "/create <room_name> - Create a new room\n"
        "/join <room_no> - Join an existing room\n"
        "/leave - Leave the current room\n"
        "/delete_account - Delete your account\n"
        "/delete_message <message_id> - Delete a message by ID\n"
        "/help - Show this help message\n";
    
    send_usage(user, usage_msg);
}

void cmd_usage_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_usage(user);
}

// 사용자 종료 함수
void cmd_quit(User *user) {
    printf("[DEBUG] cmd_quit called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    // 사용자에게 종료 메시지 전송
    const char *msg = "You have been disconnected from the server.\n";
    send_packet(
        user->sock,
        RES_MAGIC,
        PACKET_TYPE_SERVER_NOTICE,
        msg,
        (uint16_t)strlen(msg)
    );
    printf("[INFO] User %s is quitting (sock=%d)\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    // 사용자 세션 정리
    if (user->sock >= 0) {
        shutdown(user->sock, SHUT_RDWR); // 소켓 종료
        close(user->sock); // 소켓 닫기
    }
    user->sock = -1; // 소켓 번호 초기화

}

void cmd_quit_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_quit(user);
}

// 오류 메시지 전송 함수
void cmd_error(User *user, const char *msg) {
    printf("[ERROR] %s (sock=%d)\n", msg, user->sock);
    fflush(stdout); // 버퍼 비우기

    const char *report = msg ? msg : " Unknown error.\n";
    send_error(user, report);
}

// 전체 사용자에게 공지 메시지 전송 함수
void cmd_server_notice(User *user, const char *msg) {
    printf("[DEBUG] cmd_server_notice called by %s, sock=%d, msg='%s'\n", user->id, user->sock, msg ? msg : "(null)");
    fflush(stdout); // 버퍼 비우기

    if (!msg || strlen(msg) == 0) {
        char error_msg[] = " Notice message cannot be empty.\n";
        send_error(user, error_msg);
        return;
    }
}


void usage_id(User *user) {
    char *msg = "/id <new_id(nickname)>\n";
    send_usage(user, msg);
    return;
}

void usage_manager(User *user) {
    char *msg = "/manager <user_id>\n";
    send_usage(user, msg);
    return;
}

void usage_change(User *user) {
    char *msg = "/change <room_name>\n";
    send_usage(user, msg);
    return;
}

void usage_kick(User *user) {
    char *msg = "/kick <user_id>\n";
    send_usage(user, msg);
    return;
}

void usage_create(User *user) {
    char *msg = "/create <room_name>\n";
    send_usage(user, msg);
    return;
}

void usage_join(User *user) {
    char *msg = "/join <room_no>\n";
    send_usage(user, msg);
    return;
}

void usage_leave(User *user) {
    char *msg = "/leave\n";
    send_usage(user, msg);
    return;
}

void usage_delete_account(User *user) {
    char *msg = "/delete_account\n";
    send_usage(user, msg);
    return;
}

void usage_delete_message(User *user) {
    char *msg = "/delete_message <message_id>\n";
    send_usage(user, msg);
    return;
}

void usage_help(User *user) {
    char *msg = "/help <command>\n";
    send_usage(user, msg);
    return;
}


// 클라이언트 프로세스 함수
void *client_process(void *args) {
    User *user = (User *)args;

    // 1. ID 입력 루프 (패킷 기반)
    while (user->sock >= 0 && strlen(user->id) == 0) {
        // 사용자 ID 입력 요청
        char msg[] = "Enter User ID (2 ~ 20 chars) or just press ENTER for random ID: ";
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_SERVER_NOTICE,
            msg,
            (uint16_t)strlen(msg)
        );
        
        // 패킷 수신 (사용자 ID 설정 패킷만 받음)
        PacketHeader pk_header;
        char id_buffer[MAX_ID_LEN + 1] = {0}; // ID 입력 버퍼
        unsigned char cs;

        printf("[DEBUG] Waiting for user ID input from sock=%d\n", user->sock);
        fflush(stdout); // 출력 버퍼 비우기
        ssize_t header_type = recv_all(user->sock, &pk_header, sizeof(PacketHeader));
        printf("[DEBUG] Received packet header from sock=%d\n", user->sock);
        printf("[DEBUG] header_type=%zd, magic=%x, type=%d, data_len=%d\n", header_type, pk_header.magic, pk_header.type, pk_header.data_len);
        fflush(stdout); // 출력 버퍼 비우기
        if (header_type <= 0) {
            // 연결 종료 또는 에러 발생
            printf("[ERROR] User %s disconnected or error occurred while receiving ID.\n", user->id);
            fflush(stdout);
            cleanup_client_session(user);
            return NULL; // 클라이언트 세션 종료
        }
        // 네트워크 바이트 순서 -> 호스트 바이트 순서 변환
        pk_header.magic = ntohs(pk_header.magic);
        pk_header.data_len = ntohs(pk_header.data_len);

        if (pk_header.magic != REQ_MAGIC || pk_header.type != PACKET_TYPE_SET_ID) {
            // 잘못된 패킷이면 남은 데이터 버림(읽지 않음)
            if (pk_header.data_len > 0) {
                char tmpbuf[BUFFER_SIZE];
                recv_all(user->sock, tmpbuf, pk_header.data_len);
                recv_all(user->sock, &cs, 1); // 체크섬 수신
            }
            cleanup_client_session(user); // 세션 정리
            printf("[ERROR] Invalid packet received from sock=%d. Expected SET_ID packet.\n", user->sock);
            fflush(stdout); // 출력 버퍼 비우기
            return NULL; // 클라이언트 세션 종료
        }

        // data_len == 0 또는 data_len > 0 두 경우 처리
        if (pk_header.data_len == 0) {
            // 사용자가 ID 입력하지 않고 그냥 엔터를 누른 경우 랜덤 ID 생성
            snprintf(user->id, sizeof(user->id), "User%u", rand() % 10000 + 1);
            printf("[DEBUG] User ID not provided, generated random ID: %s\n", user->id);
            fflush(stdout); // 출력 버퍼 비우기
            recv_all(user->sock, &cs, 1);
        } else if (pk_header.data_len > 0 && pk_header.data_len <= MAX_ID_LEN) {
            // 사용자 ID 입력 수신
            recv_all(user->sock, id_buffer, pk_header.data_len);
            id_buffer[pk_header.data_len] = '\0';
            recv_all(user->sock, &cs, 1); // 체크섬 수신
            
            if (strlen(id_buffer) < 2 || strlen(id_buffer) > MAX_ID_LEN) {
                // ID 길이가 유효하지 않은 경우
                char error_msg[] = " Invalid ID length. Please enter 2 to 20 characters.\n";
                send_error(user, error_msg);
                continue; // 다시 입력 요청
            }
            if (find_user_by_id_unlocked(id_buffer) || db_check_user_id(id_buffer)) {
                // ID가 이미 존재하는 경우
                char error_msg[] = " ID already exists. Please choose another ID.\n";
                send_error(user, error_msg);
                continue; // 다시 입력 요청
            }
            // ID가 유효한 경우
            strncpy(user->id, id_buffer, sizeof(user->id) - 1);
            user->id[sizeof(user->id) - 1] = '\0';
        } else {
            // ID 길이가 유효하지 않은 경우
            if (pk_header.data_len > 0) {
                char error_msg[] = " Invalid ID length. Please enter 2 to 20 characters.\n";
                send_error(user, error_msg);
                continue; // 다시 입력 요청
            }
            cleanup_client_session(user); // 세션 정리
            return NULL; // 클라이언트 세션 종료
        }

        add_user(user); // 사용자 목록에 추가
        // 사용자에게 환영 메시지 전송
        char welcome_msg[BUFFER_SIZE];
        int n = snprintf(welcome_msg, sizeof(welcome_msg), " Welcome, %s! You can now join a chatroom or create one.\n", user->id);
        send_packet(
            user->sock,
            RES_MAGIC,
            PACKET_TYPE_SERVER_NOTICE,
            welcome_msg,
            (uint16_t)n
        );
        printf("[INFO] User '%s' connected with ID: %s\n", user->id, user->id);
        fflush(stdout); // 출력 버퍼 비우기
        break;
    }

    // 2. 명령/메시지 루프 (패킷 기반)
    while (user->sock >= 0) {
        PacketHeader pk_header;
        unsigned char *data_buffer = NULL;

        // 패킷 헤더 수신
        ssize_t header_type = recv_all(user->sock, &pk_header, sizeof(PacketHeader));
        if (header_type <= 0) break; // 연결 종료 또는 에러 발생

        // 네트워크 바이트 순서 -> 호스트 바이트 순서 변환
        pk_header.magic = ntohs(pk_header.magic);
        pk_header.data_len = ntohs(pk_header.data_len);

        // 매직 필드 검사
        if (pk_header.magic != REQ_MAGIC) {
            // 잘못된 패킷이면 남은 데이터 버림(읽지 않음)
            if (pk_header.data_len > 0) {
                char tmpbuf[BUFFER_SIZE];
                recv_all(user->sock, tmpbuf, pk_header.data_len);
                unsigned char cs;
                recv_all(user->sock, &cs, 1); // 체크섬 수신
            }
            continue; // 잘못된 패킷은 무시하고 다음 패킷 대기
        }

        if (pk_header.data_len > BUFFER_SIZE) {
            // 데이터 길이가 너무 긴 경우
            if (pk_header.data_len > 0) {
                // 남아 있는 데이터는 버퍼에서 버림(읽지 않음)
                char tmpbuf[BUFFER_SIZE];
                recv_all(user->sock, tmpbuf, pk_header.data_len);
                unsigned char cs;
                recv_all(user->sock, &cs, 1); // 체크섬 수신
            }
            continue; // 잘못된 패킷은 무시하고 다음 패킷 대기
        }

        // 데이터 버퍼 할당
        if (pk_header.data_len > 0) {
            data_buffer = malloc(pk_header.data_len);
            if (!data_buffer) {
                perror("malloc for data_buffer failed");
                break; // 메모리 할당 실패 시 연결 종료
            }
            ssize_t bytes_received = recv_all(user->sock, data_buffer, pk_header.data_len);
            if (bytes_received <= 0) {
                free(data_buffer);
                break; // 연결 종료 또는 에러 발생
            }
        }

        // 체크섬 수신
        unsigned char received_checksum;
        if (recv_all(user->sock, &received_checksum, 1) <= 0) {
            if (data_buffer) {
                free(data_buffer); // 메모리 해제
            }
            break; // 연결 종료 또는 에러 발생
        }


        // 패킷 타입 검사
        switch (pk_header.type) {
            case PACKET_TYPE_MESSAGE:
                printf("[DEBUG] Received PACKET_TYPE_MESSAGE from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기
                
                if (data_buffer && pk_header.data_len > 0)
                    ((char*)data_buffer)[pk_header.data_len] = '\0';

                // 대화방에 참여 중인지 확인
                if (!user->room) {
                    char error_msg[] = " You are not in a chatroom. Please join or create a room first.\n";
                    send_error(user, error_msg);
                    break;
                }
                // 메시지 내용이 비어있는지 확인
                if (!data_buffer || pk_header.data_len == 0) {
                    char error_msg[] = " Empty message cannot be sent.\n";
                    send_error(user, error_msg);
                    break;
                }

                db_insert_message(user->room, user, (const char *)data_buffer); // 데이터베이스에 메시지 저장

                // 메시지 포맷팅
                {
                    char msg[BUFFER_SIZE];
                    snprintf(msg, sizeof(msg), "[%s] %s\n", user->id, (char *)data_buffer);

                    // 대화방 참여자에게 메시지 브로드캐스트
                    broadcast_server_message_to_room(user->room, user, msg);
                    printf("[DEBUG] User %s sent message in room %s: %s\n", user->id, user->room->room_name, (char *)data_buffer);
                    fflush(stdout); // 버퍼 비우기
                    // 클라이언트 자기 자신에게도 메시지 전송(ACK용)
                    send_packet(
                        user->sock,
                        RES_MAGIC,
                        PACKET_TYPE_MESSAGE,
                        msg,
                        (uint16_t)strlen(msg)
                    );    
                }
                break;
            case PACKET_TYPE_ID_CHANGE:
                printf("[DEBUG] Received PACKET_TYPE_ID_CHANGE from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0)
                    ((char*)data_buffer)[pk_header.data_len] = '\0';

                if (!data_buffer || pk_header.data_len == 0) {
                    char error_msg[] = " ID cannot be empty.\n";
                    send_error(user, error_msg);
                    break;
                }

                {
                    // ID 변경 요청 처리
                    char new_id[MAX_ID_LEN] = {0}; // 새 ID 버퍼 초기화
                    size_t len = pk_header.data_len < sizeof(new_id) - 1 ? pk_header.data_len : sizeof(new_id) - 1;
                    memcpy(new_id, data_buffer, len);
                    new_id[len] = '\0';

                    if (strlen(new_id) < 2 || strlen(new_id) > MAX_ID_LEN) {
                        char error_msg[] = " Invalid ID length. Please enter 2 to 20 characters.\n";
                        send_error(user, error_msg);
                        break;
                    }
                    if (find_user_by_id_unlocked(new_id) || db_check_user_id(new_id)) {
                        char error_msg[BUFFER_SIZE];
                        snprintf(error_msg, sizeof(error_msg), " ID '%s' already exists. Please choose another ID.\n", new_id);
                        send_error(user, error_msg);
                        break;
                    }

                    db_update_user_id(user, new_id); // 데이터베이스에서 ID 변경
                    strncpy(user->id, new_id, sizeof(user->id) - 1);
                    user->id[sizeof(user->id) - 1] = '\0';

                    char ok[BUFFER_SIZE];
                    int n = snprintf(ok, sizeof(ok), " Your ID has been changed to '%s'.\n", user->id);
                    send_packet(
                        user->sock,
                        RES_MAGIC,
                        PACKET_TYPE_SERVER_NOTICE,
                        ok,
                        (uint16_t)n
                    );

                    printf("[INFO] User ID changed: %s -> %s\n", user->id, new_id);
                    fflush(stdout); // 버퍼 비우기
                }
                break;
            case PACKET_TYPE_CREATE_ROOM:
                printf("[DEBUG] Received PACKET_TYPE_CREATE_ROOM from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char room_name[MAX_ROOM_NAME_LEN];
                    size_t len = pk_header.data_len < sizeof(room_name) - 1 ? pk_header.data_len : sizeof(room_name) - 1;
                    memcpy(room_name, data_buffer, len);
                    room_name[len] = '\0';
                    cmd_create(user, room_name); // 대화방 생성 명령 처리
                }
                break;
            case PACKET_TYPE_JOIN_ROOM:
                printf("[DEBUG] Received PACKET_TYPE_JOIN_ROOM from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    if (pk_header.data_len == sizeof(uint32_t)) {
                        // 클라이언트가 4바이트 정수(네트워크 바이트 순서)로 대화방 번호를 보낸 경우
                        uint32_t room_no_net;
                        memcpy(&room_no_net, data_buffer, sizeof(room_no_net));
                        uint32_t room_no = ntohl(room_no_net); // 네트워크 바이트 순서 -> 호스트 바이트 순서 변환

                        char room_no_str[5];
                        snprintf(room_no_str, sizeof(room_no_str), "%u", room_no);
                        
                        cmd_join(user, room_no_str); // 대화방 참여 명령 처리
                    } else {
                        char room_no_str[5];
                        size_t len = pk_header.data_len < sizeof(room_no_str) - 1 ? pk_header.data_len : sizeof(room_no_str) - 1;
                        memcpy(room_no_str, data_buffer, len);
                        room_no_str[len] = '\0';
                        cmd_join(user, room_no_str); // 대화방 참여 명령 처리
                    }
                }
                break;
            case PACKET_TYPE_LEAVE_ROOM:
                printf("[DEBUG] Received PACKET_TYPE_LEAVE_ROOM from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기
                
                cmd_leave(user); // 대화방 나가기 명령 처리
                break;
            case PACKET_TYPE_LIST_ROOMS:
                printf("[DEBUG] Received PACKET_TYPE_LIST_ROOMS from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                cmd_rooms(user->sock); // 대화방 목록 요청 처리
                break;
            case PACKET_TYPE_LIST_USERS:
                printf("[DEBUG] Received PACKET_TYPE_LIST_USERS from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                cmd_users(user); // 사용자 목록 요청 처리
                break;
            case PACKET_TYPE_KICK_USER:
                printf("[DEBUG] Received PACKET_TYPE_KICK_USER from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char target_id[MAX_ID_LEN];
                    size_t len = pk_header.data_len < sizeof(target_id) - 1 ? pk_header.data_len : sizeof(target_id) - 1;
                    memcpy(target_id, data_buffer, len);
                    target_id[len] = '\0';
                    cmd_kick(user, target_id); // 사용자 추방 명령 처리
                }
                break;
            case PACKET_TYPE_CHANGE_ROOM_NAME:
                printf("[DEBUG] Received PACKET_TYPE_CHANGE_ROOM_NAME from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char new_room_name[MAX_ROOM_NAME_LEN];
                    size_t len = pk_header.data_len < sizeof(new_room_name) - 1 ? pk_header.data_len : sizeof(new_room_name) - 1;
                    memcpy(new_room_name, data_buffer, len);
                    new_room_name[len] = '\0';
                    cmd_change(user, new_room_name); // 대화방 이름 변경 명령 처리
                }
                break;
            case PACKET_TYPE_CHANGE_ROOM_MANAGER:
                printf("[DEBUG] Received PACKET_TYPE_CHANGE_ROOM_MANAGER from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char new_manager_id[MAX_ID_LEN];
                    size_t len = pk_header.data_len < sizeof(new_manager_id) - 1 ? pk_header.data_len : sizeof(new_manager_id) - 1;
                    memcpy(new_manager_id, data_buffer, len);
                    new_manager_id[len] = '\0';
                    cmd_manager(user, new_manager_id); // 대화방 관리자 변경 명령 처리
                }
                break;
            case PACKET_TYPE_DELETE_ACCOUNT:
                printf("[DEBUG] Received PACKET_TYPE_DELETE_ACCOUNT from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                cmd_delete_account(user); // 사용자 계정 삭제 명령 처리
                // cmd_delete_account 함수 내에서 세션 종료 처리하므로, 이후 처리를 막기 위해 return
                return NULL; // 스레드 종료
                break;
            case PACKET_TYPE_DELETE_MESSAGE:
                printf("[DEBUG] Received PACKET_TYPE_DELETE_MESSAGE from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char message_id_str[16];
                    size_t len = pk_header.data_len < sizeof(message_id_str) - 1 ? pk_header.data_len : sizeof(message_id_str) - 1;
                    memcpy(message_id_str, data_buffer, len);
                    message_id_str[len] = '\0';
                    cmd_delete_message(user, message_id_str); // 메시지 삭제 명령 처리
                }
                break;
            case PACKET_TYPE_HELP:
                printf("[DEBUG] Received PACKET_TYPE_HELP from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                cmd_help(user); // 도움말 요청 처리
                break;
            case PACKET_TYPE_USAGE:
                printf("[DEBUG] Received PACKET_TYPE_USAGE from user %s\n", user->id);
                fflush(stdout); // 버퍼 비우기

                cmd_usage(user); // 사용법 요청 처리
                break;
            case PACKET_TYPE_QUIT:
                printf("[DEBUG] Received PACKET_TYPE_QUIT from user %s\n", user->id);
                // 사용자 세션 종료 처리
                printf("[INFO] User %s (fd %d) requested to quit.\n", user->id, user->sock);
                fflush(stdout);
                cleanup_client_session(user);
                return NULL; // 스레드 종료  
            case PACKET_TYPE_ERROR:
                // 클라이언트 오류 메시지 처리
                if (data_buffer) {
                    printf("[CLIENT ERROR] from %s (sock=%d): %s\n", user->id, user->sock, (char*)data_buffer);
                    fflush(stdout); // 버퍼 비우기
                }
                break;
            case PACKET_TYPE_SET_ID:
                // 클라이언트가 ID를 설정하는 패킷 처리
                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    char new_id[MAX_ID_LEN];
                    size_t len = pk_header.data_len < sizeof(new_id) - 1 ? pk_header.data_len : sizeof(new_id) - 1;
                    memcpy(new_id, data_buffer, len);
                    new_id[len] = '\0';

                    if (strlen(new_id) < 2 || strlen(new_id) > MAX_ID_LEN) {
                        char error_msg[] = "Invalid ID length. Please enter 2 to 20 characters.\n";
                        send_error(user, error_msg);
                        break;
                    }
                    if (find_user_by_id_unlocked(new_id) || db_check_user_id(new_id)) {
                        char error_msg[BUFFER_SIZE];
                        snprintf(error_msg, sizeof(error_msg), "ID '%s' already exists. Please choose another ID.\n", new_id);
                        send_error(user, error_msg);
                        break;
                    }

                    db_update_user_id(user, new_id); // 데이터베이스에서 ID 변경
                    strncpy(user->id, new_id, sizeof(user->id) - 1);
                    user->id[sizeof(user->id) - 1] = '\0';

                    char ok[BUFFER_SIZE];
                    int n = snprintf(ok, sizeof(ok), "Your ID has been set to '%s'.\n", user->id);
                    send_packet(
                        user->sock,
                        RES_MAGIC,
                        PACKET_TYPE_SERVER_NOTICE,
                        ok,
                        (uint16_t)n
                    );

                    printf("[INFO] User ID set: %s\n", user->id);
                    fflush(stdout); // 버퍼 비우기
                }
                break;
            case PACKET_TYPE_SERVER_NOTICE:
                // 서버 공지 메시지 처리
                if (data_buffer && pk_header.data_len > 0) {
                    ((char*)data_buffer)[pk_header.data_len] = '\0';
                    printf("from %s (sock=%d): %s\n", user->id, user->sock, (char*)data_buffer);
                    fflush(stdout); // 버퍼 비우기
                }
                break;
            // 기타 알 수 없는 패킷 타입 처리
            default:
                {
                    char error_msg[BUFFER_SIZE];
                    int n = snprintf(error_msg, sizeof(error_msg), "Unknown packet type: %u\n", pk_header.type);
                    send_packet(
                        user->sock,
                        RES_MAGIC,
                        pk_header.type,
                        error_msg,
                        (uint16_t)n
                    );
                    printf("[DEBUG] Unknown packet type %d from user %s (sock=%d)\n", pk_header.type, user->id, user->sock);
                    fflush(stdout); // 버퍼 비우기
                }
                break;
        }

        if (data_buffer) free(data_buffer);
        data_buffer = NULL; // 데이터 버퍼 초기화
    }

    // 3. 세션 종료 처리
    cleanup_client_session(user);
    printf("[INFO] User %s (fd %d) session ended.\n", user->id, user->sock);
    return NULL;
}

// 서버 명령어 처리 함수
void process_server_cmd(void) {
    char cmd_buf[BUFFER_SIZE];
    if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) {
        fprintf(stderr, "Error reading command: %s\n", strerror(errno));
        fflush(stdout); // 버퍼 비우기
        return;
    }

    cmd_buf[strcspn(cmd_buf, "\r\n")] = '\0'; // 개행 문자 제거

    char *cmd = strtok(cmd_buf, " "); // 첫 번째 토큰을 명령어로 사용
    char *arg = strtok(NULL, ""); // 나머지 토큰을 인자로 사용
    
    if (!cmd || strlen(cmd) == 0) {
        printf("No command entered. Type 'help' for available commands.\n");
        fflush(stdout); // 버퍼 비우기
        return;
    }

    if (strcmp(cmd, "users") == 0) {
        server_user();
    }
    else if (strcmp(cmd, "rooms") == 0) {
        server_room();
    }
    else if (strcmp(cmd, "quit") == 0) {
        server_quit();
    }
    else if (strcmp(cmd, "user_info") == 0) {
        server_user_info_wrapper();
    }
    else if (strcmp(cmd, "room_info") == 0) {
        server_room_info_wrapper();
    }
    else if (strcmp(cmd, "recent_users") == 0) {
        int limit = 10; // 기본값 10
        if (arg) {
            limit = atoi(arg);
            if (limit <= 0) {
                printf("Limit must be a positive integer.\n");
                fflush(stdout); // 버퍼 비우기
                return;
            }
        }
        db_recent_user(limit);
    }
    else if (strcmp(cmd, "help") == 0) {
        printf("Available commands: users, rooms, user_info, room_info, recent_users, quit\n");
        fflush(stdout); // 버퍼 비우기
        return;
    }
    else {
        printf("Unknown server command. Available: users, rooms, recent_users, quit\n");
        fflush(stdout); // 버퍼 비우기
        return;
    }
    printf("> ");
    fflush(stdout); // 버퍼 비우기
}


// ================================== 메인 함수 ================================
int main() {
    srand((unsigned)time(NULL));
    db_init(); // 데이터베이스 초기화
    db_reset_all_user_connected(); // 모든 사용자 연결 상태 초기화

    // 다음 대화방 번호를 데이터베이스에서 불러와 설정
    g_next_room_no = db_get_max_room_no() + 1;
    if (g_next_room_no == 0) {
        g_next_room_no = 1; // 최소값 1로 설정
    }
    printf("[INFO] Next room number initialized to %u\n", g_next_room_no);
    fflush(stdout); // 버퍼 비우기

    int ns;
    struct sockaddr_in sin, cli;
    socklen_t clientlen = sizeof(cli);
        
    // 서버 소켓 생성
    if ((g_server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    // SO_REUSEADDR 설정
    int optval = 1;
    if (setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt");
        close(g_server_sock);
        exit(1);
    }

    // 서버 주소 설정
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORTNUM);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    // 바인딩
    if (bind(g_server_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        close(g_server_sock);
        exit(1);
    }

    // 리스닝
    if (listen(g_server_sock, 5) < 0) {
        perror("listen");
        close(g_server_sock);
        exit(1);
    }

    // epoll 인스턴스 생성 및 이벤트 배열 선언
    g_epfd = epoll_create(1);
    if (g_epfd < 0) {
        perror("epoll_create");
        close(g_server_sock);
        exit(1);
    }

    int epoll_num = 0;
    struct epoll_event ev, events[MAX_CLIENT];

    // 서버 소켓 epoll 등록 (클라이언트 접속 감지용)
    ev.events = EPOLLIN;
    ev.data.fd = g_server_sock;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_server_sock, &ev);

    // 표준입력 stdin(epoll용) 등록 (관리자 명령 입력)
    ev.events = EPOLLIN;
    ev.data.fd = 0;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, 0, &ev);

    printf("Server started.\n");
    fflush(stdout); // 버퍼 비우기

    // 메인 이벤트 루프
    while (1) {
        // 이벤트 발생한 소켓만 감지
        if ((epoll_num = epoll_wait(g_epfd, events, MAX_CLIENT, -1)) > 0) {
           for (int i = 0; i < epoll_num; i++) {
                // 1. 서버 소켓: 새 클라이언트 연결 요청 수락
                if (events[i].data.fd == g_server_sock) {
                    ns = accept(g_server_sock, (struct sockaddr *)&cli, &clientlen);
                    if (ns < 0) {
                        perror("accept");
                        continue; // 다음 이벤트로 넘어감
                    }
                    
                    // 현재 접속 중인 사용자 수 계산
                    int user_count = 0;
                    pthread_mutex_lock(&g_users_mutex);
                    User *tmp_user = g_users;
                    while (tmp_user != NULL) {
                        user_count++;
                        tmp_user = tmp_user->next;
                    }
                    pthread_mutex_unlock(&g_users_mutex);

                    if (user_count >= MAX_CLIENT) {
                        char *msg = "Server is full. Try again later.\n";
                        send_packet(
                            ns,
                            RES_MAGIC,
                            PACKET_TYPE_SERVER_NOTICE,
                            msg,
                            (uint16_t)strlen(msg)
                        );
                        printf("[INFO] Connection refused: server is full (max %d users).\n", MAX_CLIENT);
                        fflush(stdout); // 버퍼 비우기
                        close(ns);
                    } else {
                        User *user = malloc(sizeof(*user));
                        if (!user) {
                            perror("malloc for User failed");
                            close(ns);
                            continue; // 메모리 할당 실패 시 다음 이벤트로 넘어감
                        }
                        memset(user, 0, sizeof(*user));
                        user->sock = ns;
                        user->room = NULL;
                        user->pending_delete = 0; // 계정 삭제 요청 플래그 초기화
                        user->id[0] = '\0'; // ID 초기화

                        if (db_is_sock_connected(user->sock)) {
                            // DB에 같은 소켓 번호가 연결되어 있으면 강제로 연결 해제 처리
                            db_update_user_connected(user, 0); // 이전 연결을 끊었다고 표시
                            // 이제 새로 할당 가능 (continue하지 않고 아래로 진행)
                        }
                        
                        // 클라이언트 전용 스레드 생성
                        if (pthread_create(&user->thread, NULL, client_process, user) != 0) {
                            perror("pthread_create");
                            free(user); // 스레드 생성 실패 시 메모리 해제
                            close(ns);
                            continue; // 다음 이벤트로 넘어감
                        }
                        pthread_detach(user->thread); // 리소스 자동 회수
                    }
                // stdin 입력 처리 (CLI 명령)
                } else if (events[i].data.fd == 0) {
                    process_server_cmd();
                }
            }
        }
    }
    close(g_server_sock);
    db_close(); // 데이터베이스 종료
    return 0;
}
