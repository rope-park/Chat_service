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
    {"user_info", server_user_info_wrapper, "Show user info by ID"},
    {"room_info", server_room_info_wrapper, "Show room info by room name"},
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
    {"/delete_account", cmd_delete_account_wrapper, usage_delete_account, "Delete your account"},
    {"/delete_message", cmd_delete_message, usage_delete_message, "Delete a message from the chatroom"},
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

// 특정 사용자 정보 출력 함수 - 사용자 ID로 검색
void server_user_info(char *id) {
    if (!id || strlen(id) == 0) {
        printf("Usage: user_info <user_id>\n");
        return;
    }

    pthread_mutex_lock(&g_users_mutex);
    find_client_by_id(id);
    pthread_mutex_unlock(&g_users_mutex);

    db_get_user_info(id); // 데이터베이스에서 사용자 정보 가져오기
}

// typedef에서 warning
void server_user_info_wrapper() {
    char id[20];
    printf("Enter user ID to get info: ");
    fflush(stdout);
    if (fgets(id, sizeof(id), stdin) == NULL) {
        perror("fgets error");
        return;
    }
    // 개행 문자 제거
    id[strcspn(id, "\r\n")] = '\0';

    server_user_info(id); // 사용자 정보 출력 함수 호출
}

// 대화방 정보 출력 함수 - 대화방 이름으로 검색
void server_room_info(char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        printf("Usage: room_info <room_name>\n");
        return;
    }

    pthread_mutex_lock(&g_rooms_mutex);
    Room *r = find_room(room_name);
    pthread_mutex_unlock(&g_rooms_mutex);

    if (r) {
        db_get_room_info(r); // 데이터베이스에서 대화방 정보 가져오기
    } else {
        printf("Room '%s' not found.\n", room_name);
    }
}

// typedef에서 warning
void server_room_info_wrapper() {
    char room_name[32];
    printf("Enter room name to get info: ");
    fflush(stdout);
    if (fgets(room_name, sizeof(room_name), stdin) == NULL) {
        perror("fgets error");
        return;
    }
    // 개행 문자 제거
    room_name[strcspn(room_name, "\r\n")] = '\0';

    server_room_info(room_name); // 대화방 정보 출력 함수 호출
}

// 생성된 대화방 정보 출력 함수
void server_room(void) {
    db_get_all_rooms(); // 데이터베이스에서 모든 대화방 정보 가져오기
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
// 패킷 헤더 체크섬 계산 함수 - 패킷 헤더와 데이터를 XOR 연산으로 체크섬 계산
unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length) {
    unsigned char cs = 0; // 체크섬 초기화
    for (size_t i = 0; i < length; i++) {
        cs ^= header_and_data[i]; // XOR 연산으로 체크섬 계산
    }
    return cs; // 계산된 체크섬 반환
}

// 패킷 수신 함수 - 소켓 번호, 매직 넘버, 패킷 타입, 데이터 포인터, 데이터 길이를 인자로 받음
ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total_received = 0; // 총 수신된 바이트 수 초기화
    while (total_received < len) {
        ssize_t received = recv(sock, (char*)buf + total_received, len - total_received, 0);
        if (received <= 0) { // 에러 발생 또는 연결 종료
            return received; // 에러 또는 연결 종료 시 반환
        }
        total_received += received; // 수신된 바이트 수 업데이트
    }
    return total_received; // 총 수신된 바이트 수 반환
}

// 패킷 전송 함수 - 소켓 번호, 매직 넘버, 패킷 타입, 데이터 포인터, 데이터 길이를 인자로 받음
ssize_t send_packet(int sock, uint16_t magic, uint8_t type, const void *data, uint16_t data_len) {
    if (sock < 0) return -1; // 유효하지 않은 소켓 번호

    size_t packet_payload_size = sizeof(uint16_t) + sizeof(uint8_t) + data_len; // 패킷 페이로드 크기
    size_t total_packet_size = packet_payload_size + 1; // 체크섬을 위한 추가 바이트
    unsigned char *packet_buffer = malloc(total_packet_size); // 패킷 버퍼 할당
    if (!packet_buffer) {
        perror("malloc for send_packet buffer failed");
        return -1;
    }

    // 패킷 헤더 설정
    PacketHeader *header = (PacketHeader *)packet_buffer;
    header->magic = htons(magic); // 네트워크 바이트 순서로 변환
    header->type = type;
    header->data_len = htons(data_len);

    // 데이터 복사
    if (data && data_len > 0) {
        memcpy(packet_buffer + sizeof(PacketHeader), data, data_len);
    }

    // 체크섬 계산 및 추가
    packet_buffer[packet_payload_size] = calculate_checksum(packet_buffer, packet_payload_size);

    ssize_t total_sent = 0;
    ssize_t bytes_left = total_packet_size;

    while (bytes_left > 0) {
        ssize_t sent = send(sock, packet_buffer + total_sent, bytes_left, 0); // 패킷 전송
        if (sent < 0) {
            if (errno == EINTR) continue; // 인터럽트된 경우 재시도
            perror("send error");
            free(packet_buffer);
            return -1;
        }
        total_sent += sent;
        bytes_left -= sent;
    }

    free(packet_buffer); // 패킷 버퍼 해제
    return total_sent; // 전송된 바이트 수 반환
}


// 안전한 메시지 전송 함수 - 소켓 번호와 메시지 포인터를 인자로 받음
ssize_t safe_send(int sock, const char *msg) {
    printf("[DEBUG] safe_send: sock=%d, msg=%s\n", sock, msg ? msg : "(null)");
    fflush(stdout);

    if (sock < 0 || msg == NULL) {
        printf("[DEBUG] safe_send: Invalid sock or msg\n");
        fflush(stdout);
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
    printf("[DEBUG] safe_send: %s\n", msg);
    fflush(stdout);
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
        db_remove_room(room); // 데이터베이스에서 대화방 제거
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

    if (!user || user->sock < 0) {
        // 사용자 ID 설정 실패 시 연결 종료
        safe_send(user->sock, "[Server] Failed to set user ID. Disconnecting.\n");
        close(user->sock);
        free(user);
        pthread_exit(NULL);
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
                printf("[DEBUG] cmd is NULL\n");
                continue;
            }

            if (!args) args = ""; // 인자가 없는 경우 빈 문자열로 초기화
            printf("[DEBUG] cmd: %s, args: %s\n", cmd, args);
            fflush(stdout);
            
            // ID 변경
            if (strcmp(cmd, "id") == 0) {
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

            // 메시지 삭제
            else if (strcmp(cmd, "delete_message") == 0) {
                cmd_delete_message(user, args);
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
                db_insert_message(user->room, user, buf); // 데이터베이스에 메시지 저장

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
void process_server_cmd(void) {
    char cmd_buf[BUFFER_SIZE] = {0};
    if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) {
        printf("Error reading command: %s\n", strerror(errno));
        fflush(stdout); // 버퍼 비우기
        return;
    }

    // 개행 문자 제거
    cmd_buf[strcspn(cmd_buf, "\r\n")] = '\0';

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
    else if (strcmp(cmd, "help") == 0) {
        printf("Available commands: users, rooms, user_info, room_info, recent_users, quit\n");
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
    printf("[DEBUG] cmd_users called by %s, sock=%d\n", user->id, user->sock);
    fflush(stdout); // 버퍼 비우기

    char user_list[BUFFER_SIZE];
    size_t len = 0;
    user_list[0] = '\0';

    if (user->room) {
        len += snprintf(user_list + len, sizeof(user_list) - len, "[Server] Users in room %s: ", user->room->room_name);

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
        len += snprintf(user_list + len, sizeof(user_list) - len, "[Server] Connected users: ");

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

    printf("[DEBUG] cmd_users: about to call safe_send\n");
    fflush(stdout); // 버퍼 비우기
    safe_send(user->sock, user_list);
    printf("[DEBUG] cmd_users: after safe_send\n");
    fflush(stdout); // 버퍼 비우기
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
    // db_get_all_rooms(); // 데이터베이스에서 모든 대화방 정보 가져오기
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
                size_t remaining_len = BUFFER_SIZE - strlen(room_list) - 1;
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

    // DB에서 중복 체크
    pthread_mutex_lock(&g_users_mutex);
    User *cached_user = find_client_by_id_unlocked(new_id);
    pthread_mutex_unlock(&g_users_mutex);

    if (cached_user || db_check_user_id(new_id)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] ID '%s' is already taken in the database.\n", new_id);
        safe_send(user->sock, error_msg);
        return;
    }

    db_update_user_id(user, new_id); // 데이터베이스에 ID 업데이트

    // ID 변경
    printf("[INFO] User %s changed ID to %s\n", user->id, new_id);
    snprintf(user->id, sizeof(user->id), "%s", new_id);
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

    db_update_room_manager(r, target_user->id); // 데이터베이스에 방장 정보 업데이트
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
    // 기존 방 이름 중복 체크
    Room *existing_room_check = find_room_unlocked(room_name);
    if (existing_room_check != NULL && existing_room_check != r) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(user->sock, error_msg);
        return;
    }
    
    // 메모리에서 방 이름 변경
    strncpy(r->room_name, room_name, sizeof(r->room_name) - 1);
    r->room_name[sizeof(r->room_name) - 1] = '\0';
    pthread_mutex_unlock(&g_rooms_mutex);

    db_update_room_name(r, r->room_name); // 데이터베이스에 방 이름 업데이트
    
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
    
    db_remove_user_from_room(r, target_user); // 데이터베이스에서 사용자 제거
    
    
    printf("[INFO] User %s has been kicked from room %s by user %s\n", target_user->id, r->room_name, user->id);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] User '%s' has been kicked from room '%s'.\n", target_user->id, r->room_name);
    broadcast_to_room(r, target_user, "%s", success_msg);
}

// 새 대화방 생성 및 참가 함수
void cmd_create(User *creator, char *room_name) {
    printf("[DEBUG] cmd_create called: room_name='%s'\n", room_name ? room_name : "(null)");
    fflush(stdout);

    if (!room_name || strlen(room_name) == 0) {
        printf("[DEBUG] usage_create called\n");
        fflush(stdout);
        usage_create(creator);
        return;
    }
    
    // 대화방 이름 길이 제한
    if (strlen(room_name) >= sizeof(((Room *)0)->room_name)) {
        printf("[DEBUG] room name too long\n");
        fflush(stdout);
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
    pthread_mutex_unlock(&g_rooms_mutex);

    // 이미 존재하는 대화방 이름인 경우
    if (existing_room_check != NULL || db_room_name_exists(room_name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(creator->sock, error_msg);
        return;
    }

    unsigned int new_room_no = g_next_room_no++;

    // 대화방 구조체 메모리 할당
    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        pthread_mutex_unlock(&g_rooms_mutex);
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
    printf("[DEBUG] cmd_create: new_room->manager: %p, id=%s\n", (void*)new_room->manager ? new_room->manager->id: "(null)", new_room->manager ? new_room->manager->id : "(null)");
    new_room->member_count = 0;
    new_room->next = NULL;
    new_room->prev = NULL;

    // 대화방 리스트에 추가
    list_add_room_unlocked(new_room);
    room_add_member_unlocked(new_room, creator);
    if (!db_create_room(new_room)) { // db_create_room이 실패하면 0 반환하도록 수정
        // 롤백
        room_remove_member_unlocked(new_room, creator);
        list_remove_room_unlocked(new_room);
        free(new_room);
        pthread_mutex_unlock(&g_rooms_mutex);
        safe_send(creator->sock, "[Server] Failed to create room (DB error).\n");
        return;
    }
    db_add_user_to_room(new_room, creator);
    pthread_mutex_unlock(&g_rooms_mutex);

    printf("[INFO] User %s created room '%s' (ID: %u) and joined.\n", creator->id, new_room->room_name, new_room->no);

    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room '%s' (ID: %u) created and joined.\n", new_room->room_name, new_room->no);
    printf("[DEBUG] cmd_create: about to safe_send success\n");
    fflush(stdout); // 버퍼 비우기
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
    db_add_user_to_room(target_room, user); // 데이터베이스에 사용자 추가
    
    printf("[INFO] User %s joined room '%s' (ID: %u).\n", user->id, target_room->room_name, target_room->no);
    
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Joined room '%s' (ID: %u).\n", target_room->room_name, target_room->no);
    safe_send(user->sock, success_msg);
    
    db_get_room_message(target_room, user); // 대화방 메시지 로드
    
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
    
    db_remove_user_from_room(current_room, user); // 데이터베이스에서 사용자 제거
    
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

// 메시지 삭제 함수 (방장, 본인만 삭제 가능)
void cmd_delete_message(User *user, char *args) {
    if (!user || !args || strlen(args) == 0) {
        usage_delete_message(user);
        return;
    }
    int msg_id = atoi(args);
    if (msg_id <= 0) {
        safe_send(user->sock, "[Server] Invalid message ID.\n");
        return;
    }

    // 메시지 정보 조회 (sender_id, room_no)
    char sender_id[32] = {0};
    unsigned int room_no = 0;
    pthread_mutex_lock(&g_db_mutex);
    const char *sql = "SELECT sender_id, room_no FROM message WHERE id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, msg_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            strncpy(sender_id, (const char *)sqlite3_column_text(stmt, 0), sizeof(sender_id) - 1);
            room_no = sqlite3_column_int(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    if (room_no == 0) {
        safe_send(user->sock, "[Server] Message not found.\n");
        return;
    }

    Room *room = find_room_by_id(room_no);
    if (!room) {
        safe_send(user->sock, "[Server] Room not found.\n");
        return;
    }

    // 권한 체크: 방장 또는 본인만 삭제 가능
    if (strcmp(user->id, sender_id) != 0 && user != room->manager) {
        safe_send(user->sock, "[Server] Only the sender or the room manager can delete this message.\n");
        return;
    }

    int delete_result = db_remove_message_by_id(room, user, msg_id);
    if (delete_result) {
        safe_send(user->sock, "[Server] Message deleted successfully.\n");
    } else {
        safe_send(user->sock, "[Server] Failed to delete message.\n");
    }
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

void usage_delete_account(User *user) {
    char *msg = "Usage: /delete_account\n";
    safe_send(user->sock, msg);
}

void usage_delete_message(User *user) {
    char *msg = "Usage: /delete_message <message_id>\n";
    safe_send(user->sock, msg);
}

void usage_help(User *user) {
    char *msg = "Usage: /help <command>\n";
    safe_send(user->sock, msg);
}


// ================================== 메인 함수 ================================
int main() {
    db_init(); // 데이터베이스 초기화
    db_reset_all_user_connected(); // 모든 사용자 연결 상태 초기화
    g_next_room_no = db_get_max_room_no() + 1; // 다음 대화방 번호 초기화
    if (g_next_room_no == 0) {
        g_next_room_no = 1; // 최소값 1로 설정
    }
    printf("[INFO] Next room number initialized to %u\n", g_next_room_no);

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

                        if (db_is_sock_connected(user->sock)) {
                            // DB에 같은 소켓 번호가 연결되어 있으면 강제로 연결 해제 처리
                            db_update_user_connected(user, 0); // 이전 연결을 끊었다고 표시
                            // 이제 새로 할당 가능 (continue하지 않고 아래로 진행)
                        }

                        list_add_client(user);
                        
                        pthread_create(&user->thread, NULL, client_process, user);
                        pthread_detach(user->thread); // 리소스 자동 회수
                    }
                // stdin 입력 처리 (CLI 명령)
                } else if (events[i].data.fd == 0) {
                    // cmd 처리
                    process_server_cmd();
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

