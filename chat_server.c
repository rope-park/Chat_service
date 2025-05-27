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
#include "chat_server.h"
#include "db_helper.h" // 데이터베이스 관련 함수들

// ================== 전역 변수 초기화 ===================
User *g_users = NULL; // 사용자 목록
Room *g_rooms = NULL; // 대화방 목록
int g_server_sock = -1; // 서버 소켓
int g_epfd = -1; // epoll 디스크립터
unsigned int g_next_room_no = 1; // 다음 대화방 고유 번호

// Mutex 사용하여 스레드 상호 배제를 통해 안전하게 처리
pthread_mutex_t g_users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_db_mutex    = PTHREAD_MUTEX_INITIALIZER;

// command 테이블 (서버)
server_cmd_t cmd_tbl_server[] = {
    {"users", server_user, "Show all users"},
    {"recent_users", server_user, "Show recent users"},
    {"rooms", server_room, "Show all chatrooms"},
    {"quit", server_quit, "Quit server"},
    {NULL, NULL, NULL},
};

// command 테이블 (클라이언트)
client_cmd_t cmd_tbl_client[] = {
    {"/users", cmd_users_wrapper, usage_help, "List all users"},
    {"/rooms", cmd_rooms_wrapper, usage_help, "List all chatrooms"},
    {"/id", cmd_id, usage_id, "Change your ID(nickname)"},
    {"/manager", cmd_manager, usage_manager, "Change room manager"},
    {"/change", cmd_change, usage_change, "Change room name"},
    {"/kick", cmd_kick, usage_kick, "Kick users from the room"},
    {"/create", cmd_create, usage_create, "Create a new chatroom"},
    {"/join", cmd_join_wrapper, usage_join, "Join a chatroom by number"},
    {"/leave", cmd_leave_wrapper, usage_leave, "Leave current chatroom"},
    {"/delete_account", cmd_delete_account_wrapper, usage_help, "Delete your account"},
    {"/help", cmd_help_wrapper, usage_help, "Show all available commands"},
    {NULL, NULL, NULL, NULL},
};

// ==================== 기능 함수 구현 ======================
// ======== 서버부 ========
// ==== CLI ====
// 사용자 목록 정보 출력 함수
void server_user(void) {
    db_get_all_users(); // 데이터베이스에서 모든 사용자 정보 가져오기
}

// 생성된 대화방 정보 출력 함수
void server_room(void) {
    pthread_mutex_lock(&g_rooms_mutex);

    printf("%4s%20s\t%12s%8s\t%s\n", "ROOM ID", "ROOM NAME", "CREATED TIME", "#USER", "MEMBER");
    printf("==============================================================================================================\n");

    // 대화방 목록을 순회하며 정보 출력
    for (Room *r = g_rooms; r != NULL; r = r->next) {
        // 대화방 생성 시간 포맷팅
        char time_str[20];
        struct tm *tm_info = localtime((time_t *)&r->created_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        // 대화방 정보 출력
        printf("%4u%20s\t%12s%8d\t",
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

    User *member = room->members[0]; // 대화방 참여자 목록의 첫 번째 사용자
    // 대화방 참여자 목록을 순회하며 메시지 전송
    while (member != NULL) {
        if (member != sender && member->sock >= 0) {
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
    return NULL;
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
    if (room->member_count == 0) {
        // 대화방에 참여자가 없는 경우
        room->members[0] = user;
        user->room_user_next = NULL;
        user->room_user_prev = NULL;
    } else {
        // Find the first empty slot in the members array
        int idx = -1;
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (room->members[i] == NULL) {
                idx = i;
                break;
            }
        }
        if (idx != -1) {
            room->members[idx] = user;
        }
        // 연결 리스트 연결 (room_user_next/prev)
        // Find last member in the linked list
        User *last = NULL;
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (room->members[i] && room->members[i] != user) {
                if (!last || room->members[i]->room_user_next == NULL)
                    last = room->members[i];
            }
        }
        if (last) {
            last->room_user_next = user;
            user->room_user_prev = last;
        } else {
            user->room_user_prev = NULL;
        }
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

    // 배열 인덱스 땡기기
    int idx = -1;
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == user) {
            idx = i;
            break;
        }
    }
    if (idx != -1) {
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
// 사용자 스레드 처리 함수
void *client_process(void *args) {
    User *user = (User *)args;
    char buf[BUFFER_SIZE];
    ssize_t bytes_received;

    // 사용자에게 환영 메시지 전송
    safe_send(user->sock, "Welcome to the chat server!\n");

    // ID 입력 요청 루프 - 유효 ID를 입력받을 때까지 반복
    while (user->sock >= 0) {
        // 사용자에게 ID 요청
        safe_send(user->sock, "Enter User ID (2 ~ 63 chars) or just press ENTER for random ID: ");
        bytes_received = recv(user->sock, buf, sizeof(buf) - 1, 0);
        if (bytes_received <= 0) {
            // 연결 종료 또는 오류 처리
            if (bytes_received == 0) {
                printf("[INFO] User %s (fd %d) disconnected.\n", user->id, user->sock);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv");
                    printf("[ERROR] Recv error on User %s (fd %d).\n", user->id, user->sock);
                }
            }
            break;
        }
        buf[bytes_received] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0'; // 개행 문자 제거

        // ID 길이 검사
        size_t len = strlen(buf);
        // ID 미입력(ENTER 키만 누른 경우) 처리
        if (len == 0) {
            char temp_id_buf[64];
            int attempt = 0;
            int MAX_ATTEMPTS = 10;
            pthread_mutex_lock(&g_users_mutex);
            do {
                snprintf(temp_id_buf, sizeof(temp_id_buf), "User%d", rand() % 100000);
                // 현재 'user'는 g_users에 있지만 'id' 필드는 아직 'temp_id_buf'로 설정되지 않음
                // find_client_by_id_unlocked는 다른 클라이언트의 설정된 ID와 비교
                if (find_client_by_id_unlocked(temp_id_buf) == NULL) {
                    break; // 고유한 ID 발견
                }
            } while (++attempt < MAX_ATTEMPTS);
            pthread_mutex_unlock(&g_users_mutex);
            if (attempt >= MAX_ATTEMPTS) {
                // 고유한 ID 생성 실패 시 fallback 처리
                long fallback_suffix = (long)time(NULL) ^ (long)(intptr_t)user;
                snprintf(user->id, sizeof(user->id), "Guest%ld", fallback_suffix % 100000);
                user->id[sizeof(user->id) - 1] = '\0';
            }
            strncpy(user->id, temp_id_buf, sizeof(user->id) - 1);
            user->id[sizeof(user->id) - 1] = '\0';
            db_insert_user(user); // 데이터베이스에 사용자 정보 저장
            db_update_user_connected(user, 1); // 데이터베이스에 연결 상태 업데이트
            printf("[INFO] User %s (fd %d) assigned random ID.\n", user->id, user->sock);
            safe_send(user->sock, "You have been assigned a random ID.\n");
            break;
        }

        // ID 길이 검사 (2 ~ 20자)
        if (len < 2 || len > 20) {
            safe_send(user->sock, "ID should be 2 ~ 20 characters. Try again.\n");
            continue;
        }

        // ID 중복 체크
        pthread_mutex_lock(&g_users_mutex);
        User *existing_user = find_client_by_id_unlocked(buf);
        pthread_mutex_unlock(&g_users_mutex);
        if (existing_user != NULL || db_check_user_id(buf)) {
            safe_send(user->sock, "ID already in use. Try again.\n");
            continue;
        }

        // 고유 ID 발견 및 사용자 ID로 할당
        strncpy(user->id, buf, sizeof(user->id) - 1);
        user->id[sizeof(user->id) - 1] = '\0';
        db_insert_user(user); // 데이터베이스에 사용자 정보 저장
        db_update_user_connected(user, 1); // 데이터베이스에 연결 상태 업데이트
        break;
    }


    {   // 사용자 ID 설정 후 환영 메시지 전송
        char welcome_msg[BUFFER_SIZE];
        snprintf(welcome_msg, sizeof(welcome_msg),
                "[Server] Welcome! Your assigned ID(nickname) is %s.\n"
                "[Server] To change it, type: /id <new_id>\n"
                "[Server] Type /help for a list of commands.\n", user->id);
        safe_send(user->sock, welcome_msg);
        printf("[INFO] New user connected: %s (fd %d)\n", user->id, user->sock);
    }

    // 명령어/채팅 메인 루프
    while ((bytes_received = recv(user->sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[bytes_received] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0'; // 개행 문자 제거

        if (buf[0] == '/') {
            // 명령어 파싱
            char *cmd  = strtok(buf + 1, " ");
            char *args = strtok(NULL, "");
            
            if (!cmd) {
                safe_send(user->sock, "[Server] Invalid command. Type /help\n");
                continue;
            }
            
            // ID 변경
            else if (strcmp(cmd, "id") == 0) {
                cmd_id(user, args);
            }

            // 방장 변경
            else if (strcmp(cmd, "manager") == 0) {
                cmd_manager(user, args);
            }

            // 방 이름 변경
            else if (strcmp(cmd, "change") == 0) {
                cmd_change(user, args);
            }

            // 사용자 강퇴
            else if (strcmp(cmd, "kick") == 0) {
                cmd_kick(user, args);
            }
        
            // 사용자 목록
            else if (strcmp(cmd, "users") == 0) {
                cmd_users(user);
            }
        
            // 방 목록
            else if (strcmp(cmd, "rooms") == 0) {
                cmd_rooms(user->sock);
            }
        
            // 방 생성
            else if (strcmp(cmd, "create") == 0) {
                cmd_create(user, args);
            }
        
            // 방 참여
            else if (strcmp(cmd, "join") == 0) {
                cmd_join(user, args);
            }
        
            // 방 나가기
            else if (strcmp(cmd, "leave") == 0) {
                cmd_leave(user);
            }

            // 계정 삭제
            else if (strcmp(cmd, "delete_account") == 0) {
                cmd_delete_account(user);
            }

            // 도움말
            else if (strcmp(cmd, "help") == 0) {
                cmd_help(user);
            }

            else {
                safe_send(user->sock, "[Server] Unknown command. Type /help\n");
            }

        } else {
            // 일반 채팅: 방에 있으면 해당 방, 아니면 로비 브로드캐스트
            if (user->room) {
                pthread_mutex_lock(&g_db_mutex);
                db_insert_message(user->room, user, buf); // 데이터베이스에 메시지 저장
                pthread_mutex_unlock(&g_db_mutex);

                broadcast_to_room(user->room, user, "[%s] %s\n", user->id, buf);
            } else {
                broadcast_to_all(user, "[%s] %s\n", user->id, buf);
            }
        }
    }

    // 연결 해제 및 정리
    if (user->room) {
        broadcast_to_room(user->room, user, "[Server] %s has left.\n", user->id);
        room_remove_member(user->room, user);
        destroy_room_if_empty(user->room);
    }
    list_remove_client(user);
    if (user->sock >= 0) {
        shutdown(user->sock, SHUT_RDWR);
        close(user->sock);
    }
    db_update_user_connected(user, 0); // 데이터베이스에 연결 상태 업데이트
    printf("[INFO] User %s (fd %d) disconnected.\n", user->id, user->sock);


    free(user);
    pthread_exit(NULL);
}

// 서버 명령어 처리 함수
void process_server_cmd(int epfd, int server_sock) {
    char cmd_buf[BUFFER_SIZE] = {0};

    // 개행 문자 제거
    cmd_buf[strcspn(cmd_buf, "\r\n")] = '\0';

    char *cmd = strtok(cmd_buf, " "); // 첫 번째 토큰을 명령어로 사용
    char *arg = strtok(NULL, ""); // 나머지 토큰을 인자로 사용

    if (strcmp(cmd, "users") == 0) {
        server_user();
    }
    else if (strcmp(cmd, "rooms") == 0) {
        server_room();
    }
    else if (strcmp(cmd, "quit") == 0) {
        server_quit();
    }
    else if (strcmp(cmd, "help") == 0) {
        printf("Available commands: users, rooms, recent_users, quit\n");
        fflush(stdout); // 버퍼 비우기
        return;
    }
    else if (strcmp(cmd, "recent_users") == 0) {
        // 데이터베이스로부터 최근 사용자 목록 출력
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
    else {
        printf("Unknown server command: %s. Available: users, rooms, recent_users, quit\n", cmd_buf);
        fflush(stdout); // 버퍼 비우기
        return;
    }
    printf("> ");
    fflush(stdout); // 버퍼 비우기
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
        User *member = user->room->members[0];
        // 대화방 참여자 목록을 순회하며 사용자 ID 전송
        while (member) {
            strcat(user_list, member->id);
            if (member->room_user_next) {
                strcat(user_list, ", ");
            }
            member = member->room_user_next;
        }
        pthread_mutex_unlock(&g_rooms_mutex);
    } else {
        // 대화방에 참여 중이지 않은 경우 (로비)
        strcat(user_list, "[Server] Connected users: ");

        pthread_mutex_lock(&g_users_mutex);
        User *iter = g_users;
        while (iter) {
            strcat(user_list, iter->id);
            if (iter->next) {
                strcat(user_list, ", ");
            }
            iter = iter->next;
        }
        pthread_mutex_unlock(&g_users_mutex);
    }
    
    pthread_mutex_lock(&g_db_mutex);
    db_recent_user(10); // 최근 사용자 목록 업데이트
    pthread_mutex_unlock(&g_db_mutex);
    
    strcat(user_list, "\n");
    safe_send(user->sock, user_list);
}

// typedef에서 warning: type allocation error 방지
void cmd_users_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_users(user);
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

// typedef에서 warning: type allocation error 방지
void cmd_rooms_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_rooms(user->sock);
}

// ID 변경 함수
void cmd_id(User *user, char *args) {
    // 인자 유효성 검사
    if (!args ||strlen(args) == 0) {
        usage_id(user);
        return;
    }

    char *new_id = strtok(args, " ");
    // ID 길이 제한
    if (new_id == NULL || strlen(new_id) < 2 || strlen(new_id) > 20) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID must be 2 ~ 20 characters long.\n");
        safe_send(user->sock, error_msg);
        return;
    }

    // ID 중복 체크
    pthread_mutex_lock(&g_users_mutex);
    User *existing = find_client_by_id_unlocked(new_id);
    pthread_mutex_unlock(&g_users_mutex);

    if (existing && existing != user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID '%s' is already taken.\n", new_id);
        safe_send(user->sock, error_msg);
        return;
    }

    // DB에서 중복 체크
    if (db_check_user_id(new_id)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID '%s' is already taken in the database.\n", new_id);
        safe_send(user->sock, error_msg);
        return;
    }

    db_update_user_id(user, new_id); // 데이터베이스에 ID 업데이트

    // ID 변경
    printf("[INFO] User %s changed ID to %s\n", user->id, new_id);
    strncpy(user->id, new_id, sizeof(user->id) -1);
    user->id[sizeof(user->id) - 1] = '\0';

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] ID updated to %s.\n", user->id);
    safe_send(user->sock, success_msg);
}

// 방장 변경 함수
void cmd_manager(User *user, char *user_id) {
    // 사용자가 대화방에 참여 중인지 확인
    if (!user->room) {
        safe_send(user->sock, "[Server] You are not in a room.\n");
        return;
    }

    Room *r = user->room;
    // 방장 권한 확인
    if (user != r->manager) {
        safe_send(user->sock, "[Server] Only the room manager can change the manager.\n");
        return;
    }

    size_t len = strlen(user_id);
    // 인자 유효성 검사
    if (!user_id || len == 0) {
        usage_manager(user);
        return;
    }

    // 사용자 ID 검색
    User *target_user = find_client_by_id(user_id);

    // 사용자 존재 여부 확인
    if (!target_user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] No such User: '%s'.\n", user_id);
        safe_send(user->sock, error_msg);
        return;
    }
    
    // 대화방에 참여 중인지 확인
    if (target_user->room != r) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] User '%s' is not in this room.\n", target_user->id);
        safe_send(user->sock, error_msg);
        return;
    }

    // 본인에게 방장 권한 부여 시도
    if (target_user == user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] You cannot make yourself the manager.\n");
        safe_send(user->sock, error_msg);
        return;
    }

    // 방장 변경
    pthread_mutex_lock(&g_rooms_mutex);
    r->manager = target_user;
    pthread_mutex_unlock(&g_rooms_mutex);

    pthread_mutex_lock(&g_db_mutex);
    db_update_room_manager(r, target_user->id); // 데이터베이스에 방장 정보 업데이트
    pthread_mutex_unlock(&g_db_mutex);
    printf("[INFO] User %s is now the manager of room %s\n", target_user->id, r->room_name);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] User '%s' is now the manager of room '%s'.\n", target_user->id, r->room_name);
    broadcast_to_room(r, NULL, "%s", success_msg);
}

// 방 이름 변경 함수
void cmd_change(User *user, char *room_name) {
    // 사용자가 대화방에 참여 중인지 확인
    if (!user->room) {
        safe_send(user->sock, "[Server] You are not in a room.\n");
        return;
    }

    Room *r = user->room;
    // 방장 권한 확인
    if (user != r->manager) {
        safe_send(user->sock, "[Server] Only the room manager can change the room name.\n");
        return;
    }
    
    size_t len = strlen(room_name);
    // 인자 유효성 검사
    if (!room_name || len == 0) {
        usage_change(user);
        return;
    }
    
    // 대화방 이름 길이 제한
    if (len >= sizeof(r->room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name too long (max %zu characters).\n", sizeof(r->room_name) - 1);
        safe_send(user->sock, error_msg);
        return;
    }

    // 대화방 이름 변경
    pthread_mutex_lock(&g_rooms_mutex);
    strncpy(r->room_name, room_name, sizeof(r->room_name) - 1);
    r->room_name[sizeof(r->room_name) - 1] = '\0';
    pthread_mutex_unlock(&g_rooms_mutex);

    pthread_mutex_lock(&g_db_mutex);
    db_update_room_name(r, r->room_name); // 데이터베이스에 방 이름 업데이트
    pthread_mutex_unlock(&g_db_mutex);
    printf("[INFO] Room name changed to '%s' by user %s\n", r->room_name, user->id);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room name changed to '%s'.\n", r->room_name);
    broadcast_to_room(r, NULL, "%s", success_msg);
}

// 특정 유저 강제퇴장 함수
void cmd_kick(User *user, char *user_id) {
    // 사용자가 대화방에 참여 중인지 확인
    if (!user->room) {
        safe_send(user->sock, "[Server] You are not in a room.\n");
        return;
    }

    Room *r = user->room;
    // 방장 권한 확인
    if (user != r->manager) {
        safe_send(user->sock, "[Server] Only the room manger can kick users.\n");
        return;
    }

    // 인자 유효성 검사
    if (!user_id || strlen(user_id) == 0) {
        usage_kick(user);
        return;
    }
    
    // 사용자 ID 검색
    pthread_mutex_lock(&g_users_mutex);
    User *target_user = find_client_by_id_unlocked(user_id);
    pthread_mutex_unlock(&g_users_mutex);

    // 사용자 존재 여부 확인
    if (!target_user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] No such User: '%s'.\n", user_id);
        safe_send(user->sock, error_msg);
        return;
    }

    // 대화방에 참여 중인지 확인
    if (target_user->room != r) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] User '%s' is not in this room.\n", target_user->id);
        safe_send(user->sock, error_msg);
        return;
    }

    // 본인에게 강퇴 시도
    if (target_user == user) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] You cannot kick yourself.\n");
        safe_send(user->sock, error_msg);
        return;
    }

    // 강퇴된 사용자에게 메시지 전송
    safe_send(target_user->sock, "[Server] You have been kicked from the room.\n");

    // 대화방에서 사용자 제거
    room_remove_member(r, target_user);
    destroy_room_if_empty(r); // 대화방이 비어있으면 제거
    pthread_mutex_lock(&g_db_mutex);
    db_remove_user_from_room(r, target_user); // 데이터베이스에서 사용자 제거
    db_update_room_member_count(r); // 데이터베이스에서 대화방 참여자 수 업데이트
    pthread_mutex_unlock(&g_db_mutex);
    printf("[INFO] User %s has been kicked from room %s by user %s\n", target_user->id, r->room_name, user->id);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] User '%s' has been kicked from room '%s'.\n", target_user->id, r->room_name);
    broadcast_to_room(r, target_user, "%s", success_msg);
}

// 새 대화방 생성 및 참가 함수
void cmd_create(User *creator, char *room_name) {
    // 대화방 이름 유효성 검사
    if (!room_name || strlen(room_name) == 0) {
        usage_create(creator);
        return;
    }
    
    // 대화방 이름 길이 제한
    if (strlen(room_name) >= sizeof(((Room *)0)->room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name too long (max %zu characters).\n", sizeof(((Room*)0)->room_name) - 1);
        safe_send(creator->sock, error_msg);
        return;
    }
    
    // 이미 대화방에 참여 중인지 확인
    if (creator->room != NULL) {
        safe_send(creator->sock, "[Server] You are already in a room. Please /leave first.\n");
        return;
    }

    // 대화방 이름 중복 체크
    pthread_mutex_lock(&g_rooms_mutex);
    Room *existing_room_check = find_room_unlocked(room_name);
    if (existing_room_check != NULL) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(creator->sock, error_msg);
        return;
    }

    unsigned int new_room_no = g_next_room_no++;

    // 대화방 구조체 메모리 할당
    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new room failed");
        safe_send(creator->sock, "[Server] Failed to create room.\n");
        return;
    }

    strncpy(new_room->room_name, room_name, sizeof(new_room->room_name) - 1);
    new_room->room_name[sizeof(new_room->room_name) - 1] = '\0';
    new_room->no = new_room_no;
    new_room->created_time = time(NULL);
    memset(new_room->members, 0, sizeof(new_room->members));
    new_room->manager = creator;
    new_room->member_count = 1;
    new_room->next = NULL;
    new_room->prev = NULL;

    // 대화방 리스트에 추가
    list_add_room_unlocked(new_room);
    // 대화방에 사용자 추가
    room_add_member(new_room, creator);
    pthread_mutex_unlock(&g_rooms_mutex);
    printf("[INFO] User %s created room '%s' (ID: %u) and joined.\n", creator->id, new_room->room_name, new_room->no);

    pthread_mutex_lock(&g_db_mutex);
    db_create_room(new_room); // 데이터베이스에 대화방 정보 저장
    pthread_mutex_unlock(&g_db_mutex);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room '%s' (ID: %u) created and joined.\n", new_room->room_name, new_room->no);
    safe_send(creator->sock, success_msg);
}

// 특정 대화방 참여 함수
void cmd_join(User *user, char *room_no_str) {
    // 대화방 이름 유효성 검사
    if (!room_no_str || strlen(room_no_str) == 0) {
        usage_join(user);
        return;
    }

    // 대화방에 이미 참여 중인지 확인
    if (user->room != NULL) {
        safe_send(user->sock, "[Server] You are already in a room. Please /leave first.\n");
        return;
    }

    char *endptr;
    long num_id = strtol(room_no_str, &endptr, 10);

    // 대화방 번호 유효성 검사
    if (*endptr != '\0' || num_id <= 0 || num_id >= g_next_room_no || num_id > 0xFFFFFFFF) {
        safe_send(user->sock, "[Server] Invalid room ID. Please use a valid positive number.\n");
        return;
    }

    unsigned int room_no_to_join = (unsigned int)num_id;

    // 대화방 찾기
    Room *target_room = find_room_by_id(room_no_to_join);
    if (!target_room) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room with ID %u not found.\n", room_no_to_join);
        safe_send(user->sock, error_msg);
        return;
    }

    room_add_member(target_room, user);
    printf("[INFO] User %s joined room '%s' (ID: %u).\n", user->id, target_room->room_name, target_room->no);
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Joined room '%s' (ID: %u).\n", target_room->room_name, target_room->no);
    safe_send(user->sock, success_msg);
    
    pthread_mutex_lock(&g_db_mutex);
    db_get_room_message(target_room, user); // 대화방 메시지 로드
    db_update_room_member_count(target_room); // 데이터베이스에 대화방 참여자 수 업데이트
    pthread_mutex_unlock(&g_db_mutex);

    broadcast_to_room(target_room, user, "[Server] %s joined the room.\n", user->id);
    return;
}

// typedef에서 warning: type allocation error 방지
void cmd_join_wrapper(User *user, char *args) {
    if (!args) {
        usage_join(user);
        return;
    }
    cmd_join(user, args);
}

// 현재 대화방 나가기 함수
void cmd_leave(User *user) {
    // 대화방에 미참여한 경우
    if (user->room == NULL) {
        safe_send(user->sock, "[Server] You are not in any room.\n");
        return;
    }

    Room *current_room = user->room;
    broadcast_to_room(current_room, user, "[Server] %s left the room.\n", user->id);
    // 대화방에서 사용자 제거
    room_remove_member(current_room, user);
    // 데이터베이스에 대화방 참여자 수 업데이트
    pthread_mutex_lock(&g_db_mutex);
    db_update_room_member_count(current_room);
    pthread_mutex_unlock(&g_db_mutex);

    // 퇴장 메시지 전송
    printf("[INFO] User %s left room '%s' (ID: %u).\n", user->id, current_room->room_name, current_room->no);
    safe_send(user->sock, "[Server] You left the room.\n");
    // 대화방이 비어있으면 제거
    destroy_room_if_empty(current_room);
}

// typedef에서 warning: type allocation error 방지
void cmd_leave_wrapper(User *user, char *args) {
    (void)args;
    cmd_leave(user);
}

// 사용자 계정 삭제 함수
void cmd_delete_account(User *user) {
    // 사용자 대화방에서 나가기
    if (user->room) {
        cmd_leave(user);
    }
    // 사용자에게 계정 삭제 확인 메시지 전송
    safe_send(user->sock, "[Server] Your account is being deleted. You will be disconnected.\n");

    // 데이터베이스에서 사용자 정보 삭제
    pthread_mutex_lock(&g_db_mutex);
    db_remove_user(user); // 데이터베이스에서 사용자 정보 완전히 삭제
    pthread_mutex_unlock(&g_db_mutex);

    // 계정 삭제 완료 메시지 전송
    safe_send(user->sock, "[Serv송r] Your account has been deleted.\n");

    // 사용자 목록에서 제거
    list_remove_client(user);

    // 소켓 종료 및 메모리 해제
    if (user->sock >= 0) {
        shutdown(user->sock, SHUT_RDWR);
        close(user->sock);
    }
    
    printf("[INFO] User %s has deleted their account and disconnected.\n", user->id);
    
    free(user);
}

// typedef에서 warning: type allocation error 방지
void cmd_delete_account_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_delete_account(user);
}

// 도움말 출력 함수
void cmd_help(User *user) {
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

// typedef에서 warning: type allocation error 방지
void cmd_help_wrapper(User *user, char *args) {
    (void)args; // 사용하지 않는 인자
    cmd_help(user);
}

void usage_id(User *user) {
    char *msg = "Usage: /id <new_id(nickname)>\n";
    safe_send(user->sock, msg);
}

void usage_manager(User *user) {
    char *msg = "Usage: /manager <user_id>\n";
    safe_send(user->sock, msg);
}

void usage_change(User *user) {
    char *msg = "Usage: /change <room_name>\n";
    safe_send(user->sock, msg);
}

void usage_kick(User *user) {
    char *msg = "Usage: /kick <user_id>\n";
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
    char *msg = "Usage: /leave\n";
    safe_send(user->sock, msg);
}

void usage_help(User *user) {
    char *msg = "Usage: /help <command>\n";
    safe_send(user->sock, msg);
}


// ================================== 메인 함수 ================================
int main() {
    db_init(); // 데이터베이스 초기화

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
        exit(1);
    }

    // 리스닝
    if (listen(g_server_sock, 5) < 0) {
        perror("listen");
        exit(1);
    }

    // epoll 인스턴스 생성 및 이벤트 배열 선언
    g_epfd = epoll_create(1);
    struct epoll_event ev, events[MAX_CLIENT];
    int epoll_num = 0;
    
    // 서버 소켓 epoll 등록 (클라이언트 접속 감지용)
    ev.events = EPOLLIN;
    ev.data.fd = g_server_sock;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_server_sock, &ev);

    // 표준입력 stdin(epoll용) 등록 (관리자 명령 입력)
    ev.events = EPOLLIN;
    ev.data.fd = 0;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, 0, &ev);

    printf("Server started.\n");

    // 메인 이벤트 루프
    while (1) {
        // 이벤트 발생한 소켓만 감지
        if ((epoll_num = epoll_wait(g_epfd, events, MAX_CLIENT, -1)) > 0) {
           for (int i = 0; i < epoll_num; i++) {
                // 서버 소켓: 새 클라이언트 연결 요청 수락
                if (events[i].data.fd == g_server_sock) {
                    ns = accept(g_server_sock, (struct sockaddr *)&cli, &clientlen);
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
                        safe_send(ns, msg);
                        close(ns);
                    } else {
                        User *user = malloc(sizeof(*user));
                        memset(user, 0, sizeof(*user));
                        user->sock = ns;
                        user->room = NULL;
                        list_add_client(user);
                        
                        pthread_create(&user->thread, NULL, client_process, user);
                        pthread_detach(user->thread); // 리소스 자동 회수
                    }
                // stdin 입력 처리 (CLI 명령)
                } else if (events[i].data.fd == 0) {
                    // cmd 처리
                    process_server_cmd(g_epfd, g_server_sock);
                }
                printf("> ");
                fflush(stdout);
            }
        }
    }
    close(g_server_sock);
    db_close(); // 데이터베이스 종료
    return 0;
}

