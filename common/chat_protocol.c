#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <arpa/inet.h>
#include "chat_protocol.h"

// ============ 공통 유틸리티 함수 구현 ============
// 패킷 수신 함수 - 소켓 번호, 매직 넘버, 패킷 타입, 데이터 포인터, 데이터 길이를 인자로 받음
ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total_received = 0; // 총 수신된 바이트 수 초기화
    while (total_received < len) {
        ssize_t received = recv(sock, (char*)buf + total_received, len - total_received, 0);
        if (received <= 0) { // 에러 발생 또는 연결 종료
            return received;
        }
        total_received += received; // 수신된 바이트 수 업데이트
    }
    return (ssize_t)total_received;
}

// 패킷 헤더 체크섬 계산 함수 - 패킷 헤더와 데이터를 XOR 연산으로 체크섬 계산
unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length) {
    unsigned char cs = 0; // 체크섬 초기화
    for (size_t i = 0; i < length; i++) {
        cs ^= header_and_data[i]; // XOR 연산으로 체크섬 계산
    }
    return cs; // 계산된 체크섬 반환
}

// 패킷 전송 함수 - 소켓 번호, 매직 넘버, 패킷 타입, 데이터 포인터, 데이터 길이를 인자로 받음
ssize_t send_packet(int sock, uint16_t magic, uint8_t type, const void *data, uint16_t data_len) {
    PacketHeader header;
    header.magic = htons(magic); // 네트워크 바이트 순서로 변환
    header.type = type;
    header.data_len = htons(data_len); // 네트워크 바이트 순서로 변환
    if (sock < 0) return -1; // 유효하지 않은 소켓 번호

    size_t packet_payload_size = sizeof(PacketHeader) + data_len; // 패킷 페이로드 크기
    size_t total_packet_size = packet_payload_size + 1; // 체크섬을 위한 추가 바이트

    unsigned char *packet_buffer = malloc(total_packet_size); // 패킷 버퍼 할당
    if (!packet_buffer) {
        fprintf(stderr, "malloc for send_packet buffer failed");
        return -1;
    }
    memcpy(packet_buffer, &header, sizeof(PacketHeader));
    // 데이터 복사
    if (data && data_len > 0) {
        memcpy(packet_buffer + sizeof(PacketHeader), data, data_len);
    }

    // 체크섬 계산 및 추가
    packet_buffer[packet_payload_size] = calculate_checksum(packet_buffer, packet_payload_size);

    ssize_t total_sent = 0;
    ssize_t bytes_left = (ssize_t)total_packet_size;

    while (bytes_left > 0) {
        ssize_t sent = send(sock, packet_buffer + total_sent, (size_t)bytes_left, 0); // 패킷 전송
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
