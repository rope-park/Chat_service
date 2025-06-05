// common/chat_protocol.h - 채팅 프로토콜 정의 헤더
#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

// ======== 패킷 헤더 구조체 정의 ========
#pragma pack(push, 1) // 패딩 없이 구조체 정렬
typedef struct {
    uint16_t magic;        // 패킷 매직 넘버
    uint8_t type;          // 패킷 타입
    uint16_t data_len;     // 데이터 길이
} PacketHeader;
#pragma pack(pop) // 원래 구조체 정렬로 복원

// 매직 넘버 상수 - 요청/응답 구분
#define REQ_MAGIC 0x5a5a
#define RES_MAGIC 0xa5a5

// ======== 패킷 타입 열거형 정의 ========
typedef enum {
    //— 요청 패킷 타입 —
    PACKET_TYPE_MESSAGE            = 1,  // 일반 채팅 메시지 전송
    PACKET_TYPE_ID_CHANGE          = 2,  // ID 변경 요청
    PACKET_TYPE_CREATE_ROOM        = 3,  // 대화방 생성 요청
    PACKET_TYPE_JOIN_ROOM          = 4,  // 대화방 입장 요청
    PACKET_TYPE_LEAVE_ROOM         = 5,  // 대화방 퇴장 요청
    PACKET_TYPE_LIST_ROOMS         = 6,  // 대화방 목록 요청
    PACKET_TYPE_LIST_USERS         = 7,  // 사용자 목록 요청
    PACKET_TYPE_KICK_USER          = 8,  // 사용자 강퇴 요청
    PACKET_TYPE_CHANGE_ROOM_NAME   = 9,  // 방 이름 변경 요청
    PACKET_TYPE_CHANGE_ROOM_MANAGER= 10, // 방장 변경 요청
    PACKET_TYPE_DELETE_ACCOUNT     = 11, // 계정 삭제 요청
    PACKET_TYPE_DELETE_MESSAGE     = 12, // 메시지 삭제 요청
    PACKET_TYPE_HELP               = 13, // 도움말 요청
    PACKET_TYPE_USAGE              = 14, // 명령 사용법 요청
    PACKET_TYPE_QUIT               = 15, // 클라이언트 종료 요청

    //— 응답 패킷 타입 —
    PACKET_TYPE_ERROR              = 100, // 에러 응답
    PACKET_TYPE_SET_ID             = 101, // ID 변경 완료 응답
    PACKET_TYPE_SERVER_NOTICE      = 102, // 서버 공지
    // (필요 시 기능 추가 가능)
} PacketType;

// ======== 함수 프로토타입 ========
ssize_t recv_all(int sock, void *buf, size_t len); // 지정된 길이만큼 정확히 recv()하도록 보장하는 함수
ssize_t send_packet(int sock, 
                    uint16_t magic,
                    uint8_t type,
                    const void *data,
                    uint16_t data_len
                ); // 패킷 전송 함수

unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length); // 체크섬 계산 함수

#endif // CHAT_PROTOCOL_H