#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <errno.h>
#include "../common/chat_protocol.h"

#define SERVER_IP   "127.0.0.1"   // 기본 서버 IP (필요에 따라 변경)
#define SERVER_PORT 9000          // 기본 서버 포트 (필요에 따라 변경)
#define BUFFER_SIZE 2048          // 송수신 버퍼 크기

// 클라이언트 상태를 나타내는 enum
typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTED
} ClientState;

// 클라이언트 정보 구조체
typedef struct {
    int sockfd;                  // 서버와 연결된 소켓 디스크립터
    ClientState state;           // 현재 연결 상태
    char user_id[20];            // 사용자 ID (닉네임)
} ChatClient;

// ===== 함수 프로토타입 =====

// 클라이언트 초기화 (소켓 생성, 비밀번호 등)
// void client_init(ChatClient *client); // 필요에 따라 구현

// 서버에 연결 시도
int client_connect_to_server(ChatClient *client, const char *server_ip, int server_port);

// 사용자로부터 닉네임 입력 받아 설정
void client_set_user_id(ChatClient *client);

// 서버로 패킷 전송
int client_send_packet(ChatClient *client, uint8_t type, const void *data, uint16_t data_len);

// 서버가 보내오는 데이터를 처리
int client_receive_message(ChatClient *client);

// 메인 루프 (입력/출력을 select()로 감시)
void client_event_loop(ChatClient *client);

// 클라이언트 종료 처리 (소켓 닫기 등)
void client_cleanup(ChatClient *client);

#endif // CHAT_CLIENT_H
