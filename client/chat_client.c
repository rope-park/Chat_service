#include "chat_client.h"

static void print_usage(const char *progname) {
    printf("Usage: %s <server_ip> <server_port>\n", progname);
    printf("  기본값: server_ip=%s, server_port=%d\n", SERVER_IP, SERVER_PORT);
}

// 서버 연결 함수 - 연결 성공 시 1, 실패 시 0을 반환
int client_connect_to_server(ChatClient *client, const char *server_ip, int server_port) {
    struct sockaddr_in serv_addr;

    // 소켓 생성
    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        perror("[Client] socket 생성 오류");
        return 0;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);

    // IP 문자열을 이진형으로 변환
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[Client] 잘못된 IP 주소: %s\n", server_ip);
        close(client->sockfd);
        return 0;
    }

    // 서버 연결 시도
    if (connect(client->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Client] 서버 연결 실패");
        close(client->sockfd);
        return 0;
    }

    client->state = STATE_CONNECTED;
    printf("[Client] 서버에 연결되었습니다: %s:%d\n", server_ip, server_port);
    return 1;
}

// 사용자 ID 설정 함수 - 사용자에게서 ID를 입력받아 client->user_id에 저장
void client_set_user_id(ChatClient *client) {
    printf("원하는 닉네임을 입력하세요 (2~20자, 엔터시 랜덤): ");
    fflush(stdout);

    if (fgets(client->user_id, sizeof(client->user_id), stdin) == NULL) {
        strcpy(client->user_id, "Anonymous");
    } else {
        // 개행 문자 제거
        size_t len = strlen(client->user_id);
        if (len > 0 && client->user_id[len - 1] == '\n') {
            client->user_id[len - 1] = '\0';
        }
        if (strlen(client->user_id) == 0) {
            strcpy(client->user_id, "Anonymous");
        }
    }

    printf("[Client] 닉네임 설정: %s\n", client->user_id);

    // 서버에 ID 패킷 전송
    printf("[DEBUG] send_packet: id='%s', len=%zu\n", client->user_id, strlen(client->user_id));
    fflush(stdout);
    ssize_t sent = send_packet(client->sockfd, REQ_MAGIC, PACKET_TYPE_SET_ID, client->user_id, (uint16_t)strlen(client->user_id));
    printf("[DEBUG] send_packet return: %zd\n", sent);
    fflush(stdout);
}

// 서버로 패킷 전송 함수 - 전송 성공 시 1, 실패 시 0 반환
int client_send_packet(ChatClient *client, uint8_t type, const void *data, uint16_t data_len) {
    if (client->state != STATE_CONNECTED) {
        fprintf(stderr, "[Client] 아직 서버에 연결되지 않았습니다.\n");
        return 0;
    }
    
    return send_packet(client->sockfd, REQ_MAGIC, type, data, data_len);
}

// 서버로 메시지(일반 채팅)를 패킷 전송 함수
int client_send_message(ChatClient *client, const char *message) {
    if (client->state != STATE_CONNECTED) {
        fprintf(stderr, "[Client] 아직 서버에 연결되지 않았습니다.\n");
        return 0;
    }

    // 메시지 길이 체크
    size_t msg_len = strlen(message);
    if (msg_len == 0) {
        fprintf(stderr, "[Client] 메시지는 1~%d자 사이여야 합니다.\n", BUFFER_SIZE - 1);
        return 0;
    }
    if (strncmp(message, "/join ", 6) == 0) {
        const char *room_no = message + 6;
        return client_send_packet(client, PACKET_TYPE_JOIN_ROOM, room_no, (uint16_t)strlen(room_no));
    } else if (strncmp(message, "/leave", 6) == 0) {
        return client_send_packet(client, PACKET_TYPE_LEAVE_ROOM, NULL, 0);
    } else if (strncmp(message, "/quit", 5) == 0) {
        return client_send_packet(client, PACKET_TYPE_QUIT, NULL, 0);
    } else if (strncmp(message, "/users", 6) == 0) {
        return client_send_packet(client, PACKET_TYPE_LIST_USERS, NULL, 0);
    } else if (strncmp(message, "/rooms", 6) == 0) {
        return client_send_packet(client, PACKET_TYPE_LIST_ROOMS, NULL, 0);
    } else if (strncmp(message, "/id ", 4) == 0) {
        const char *new_id = message + 4;
        return client_send_packet(client, PACKET_TYPE_ID_CHANGE, new_id, (uint16_t)strlen(new_id));
    } else if (strncmp(message, "/create ", 8) == 0) {
        const char *room_name = message + 8;
        return client_send_packet(client, PACKET_TYPE_CREATE_ROOM, room_name, (uint16_t)strlen(room_name));
    } else if (strncmp(message, "/kick ", 6) == 0) {
        const char *target_user = message + 6;
        return client_send_packet(client, PACKET_TYPE_KICK_USER, target_user, (uint16_t)strlen(target_user));
    } else if (strncmp(message, "/delete_account", 15) == 0) {
        return client_send_packet(client, PACKET_TYPE_DELETE_ACCOUNT, NULL, 0);
    } else if (strncmp(message, "/delete_message ", 16) == 0) {
        const char *message_id_str = message + 16;
        return client_send_packet(client, PACKET_TYPE_DELETE_MESSAGE, message_id_str, (uint16_t)strlen(message_id_str));
    } else if (strncmp(message, "/help", 5) == 0) {
        return client_send_packet(client, PACKET_TYPE_HELP, NULL, 0);
    } else if (strncmp(message, "/change ", 7) == 0) {
        const char *new_room_name = message + 7;
        return client_send_packet(client, PACKET_TYPE_CHANGE_ROOM_NAME, new_room_name, (uint16_t)strlen(new_room_name));
    } else if (strncmp(message, "/manager ", 9) == 0) {
        const char *new_manager_id = message + 9;
        return client_send_packet(client, PACKET_TYPE_CHANGE_ROOM_MANAGER, new_manager_id, (uint16_t)strlen(new_manager_id));
    } else {
        // 일반 메시지 전송
        if (msg_len > BUFFER_SIZE - 1) {
            fprintf(stderr, "[Client] 메시지는 최대 %d자까지 입력할 수 있습니다.\n", BUFFER_SIZE - 1);
            return 0;
        }
    }
    // 서버로 메시지 전송
    return client_send_packet(client, PACKET_TYPE_MESSAGE, message, (uint16_t)msg_len);
}

// 서버로부터 패킷 수신 및 처리 함수 - 읽기 성공 시 1, 서버가 연결을 종료했거나 오류 시 0을 반환
int client_receive_message(ChatClient *client) {
    PacketHeader header;
    unsigned char data_buffer[BUFFER_SIZE];
    ssize_t n = recv_all(client->sockfd, &header, sizeof(PacketHeader));
    if (n <= 0) {
        perror("[Client] recv_all 오류");
        return 0;
    }
    header.magic = ntohs(header.magic);
    header.data_len = ntohs(header.data_len);
    if (header.data_len > 0) {
        recv_all(client->sockfd, data_buffer, header.data_len);
        data_buffer[header.data_len] = '\0'; 
    }
    unsigned char checksum;
    if (recv_all(client->sockfd, &checksum, 1) <= 0) {
        perror("[Client] 체크섬 수신 오류");
        return 0;
    }

    // 패킷 타입별 처리
    switch (header.type) {
        case PACKET_TYPE_MESSAGE: 
        case PACKET_TYPE_HELP:
        case PACKET_TYPE_LIST_USERS:
        case PACKET_TYPE_LIST_ROOMS:
            printf("[Server] %s\n", data_buffer);
            break;
        case PACKET_TYPE_ID_CHANGE: 
            printf("[Server] ID가 '%s'로 변경되었습니다.\n", data_buffer);
            strncpy(client->user_id, (const char *)data_buffer, sizeof(client->user_id) - 1);
            client->user_id[sizeof(client->user_id) - 1] = '\0'; // 널 종료
            break;
        case PACKET_TYPE_CREATE_ROOM: 
            printf("[Server] 대화방 '%s'이(가) 생성되었습니다.\n", data_buffer);
            break;
        case PACKET_TYPE_JOIN_ROOM:
            printf("[Server] 대화방 '%s'에 참여했습니다.\n", data_buffer);
            break;
        case PACKET_TYPE_LEAVE_ROOM:
            printf("[Server] 대화방을 나갔습니다.\n");
            break;
        case PACKET_TYPE_KICK_USER:
            printf("[Server] '%s' 사용자가 강퇴되었습니다.\n", data_buffer);
            break;
        case PACKET_TYPE_DELETE_ACCOUNT:
            printf("[Server] 계정이 삭제되었습니다.\n");
            client->state = STATE_DISCONNECTED; // 연결 상태 변경
            close(client->sockfd); // 소켓 닫기
            break;
        case PACKET_TYPE_DELETE_MESSAGE:
            printf("[Server] 메시지가 삭제되었습니다: %s\n", data_buffer);
            break;
        case PACKET_TYPE_CHANGE_ROOM_NAME:
            printf("[Server] 대화방 이름이 '%s'로 변경되었습니다.\n", data_buffer);
            break;
        case PACKET_TYPE_CHANGE_ROOM_MANAGER:
            printf("[Server] 대화방 관리자 ID가 '%s'로 변경되었습니다.\n", data_buffer);
            break;
        case PACKET_TYPE_SERVER_NOTICE: 
            printf("[Server Notice] %s\n", data_buffer);
            break;
        case PACKET_TYPE_SET_ID: 
            printf("[Server] ID가 '%s'로 설정되었습니다.\n", data_buffer);
            strncpy(client->user_id, (const char *)data_buffer, sizeof(client->user_id) - 1);
            client->user_id[sizeof(client->user_id) - 1] = '\0';
            break;
        case PACKET_TYPE_QUIT:
            printf("[Server] 클라이언트가 종료 요청을 보냈습니다. 연결을 종료합니다.\n");
            client->state = STATE_DISCONNECTED;
            close(client->sockfd);
            break;
        case PACKET_TYPE_USAGE:
            printf("[Server] 사용법: %s\n", data_buffer);
            break;
        case PACKET_TYPE_ERROR:
            printf("[Server Error] %s\n", data_buffer);
            break;
        default:
            fprintf(stderr, "[Client] 알 수 없는 패킷 타입: %d\n", header.type);
            break;
    }
    fflush(stdout);
    return 1;
}

// 이벤트 루프 함수 - 입력(stdin)과 서버 소켓(sockfd)을 select()로 감시 및 사용자 입력이 있으면 서버로 전송, 서버 응답이 있으면 화면에 출력
void client_event_loop(ChatClient *client) {
    fd_set read_fds;
    int max_fd = (client->sockfd > 0 ? client->sockfd : 0);
    char input_buf[BUFFER_SIZE];

    while (client->state == STATE_CONNECTED) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(client->sockfd, &read_fds);

        int select_ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (select_ret < 0) {
            if (errno == EINTR) continue;
            perror("[Client] select 오류");
            break;
        }

        // 1) 사용자가 키보드로 입력한 내용이 있는 경우
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) {
                // EOF 또는 오류
                printf("[Client] 입력 스트림이 닫혔습니다.\n");
                break;
            }

            // 사용자가 “/quit” 을 입력하면 종료
            if (strncmp(input_buf, "/quit", 5) == 0) {
                printf("[Client] 채팅을 종료합니다.\n");
                break;
            }

            // 개행 문자 제거
            size_t len = strlen(input_buf);
            if (len > 0 && input_buf[len - 1] == '\n') {
                input_buf[len - 1] = '\0';
            }
            // 입력된 메시지를 서버로 전송
            client_send_message(client, input_buf);
        }

        // 2) 서버로부터 수신한 내용이 있는 경우
        if (FD_ISSET(client->sockfd, &read_fds)) {
            if (!client_receive_message(client)) {
                // 서버가 연결을 끊었거나 오류 발생 시 루프 탈출
                break;
            }
        }
    }

    // 루프 종료 시 연결 종료 처리
    client_cleanup(client);
}

// 소켓 종료 및 상태 변경 함수
void client_cleanup(ChatClient *client) {
    if (client->state == STATE_CONNECTED) {
        close(client->sockfd);
        client->state = STATE_DISCONNECTED;
        printf("[Client] 소켓을 닫고 종료합니다.\n");
    }
}

// ===== 메인 함수 =====
int main(int argc, char *argv[]) {
    ChatClient client;
    const char *server_ip   = SERVER_IP;
    int         server_port = SERVER_PORT;

    // 인자 처리: ./chat_client 192.168.0.10 9000
    if (argc >= 2) {
        server_ip = argv[1];
    }
    if (argc >= 3) {
        server_port = atoi(argv[2]);
    }
    if (argc > 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    client.state = STATE_DISCONNECTED;
    memset(client.user_id, 0, sizeof(client.user_id));

    // 서버 연결 시도
    if (!client_connect_to_server(&client, server_ip, server_port)) {
        return EXIT_FAILURE;
    }

    // 닉네임 입력 및 전송
    client_set_user_id(&client);
    // 이벤트 루프 시작
    client_event_loop(&client);

    return EXIT_SUCCESS;
}
