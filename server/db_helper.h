#ifndef DB_HELPER_H
#define DB_HELPER_H

// ===== 구조체 전방 선언 =====
typedef struct User User; // 사용자 구조체
typedef struct Room Room; // 대화방 구조체

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern sqlite3 *db; // SQLite 데이터베이스 핸들

// 데이터베이스 초기화 및 종료 함수
int db_init();
void db_close();

// ===== 사용자 관련 함수 =====
void db_reset_all_user_connected();                         // 모든 사용자 연결 상태 초기화
void db_insert_user(User *user);                            // 새로운 사용자 생성
void db_remove_user(User *user);                            // 기존 사용자 삭제
void db_update_user_id(User *user, const char *new_id);     // 사용자 ID 변경
void db_update_user_connected(User *user, int status);      // 사용자 연결 상태 업데이트(1: 연결, 0: 연결 해제)

int db_is_sock_connected(int sock);                         // 소켓 번호와 연결 상태로 사용자 검색
int db_check_user_id(const char *user_id);                  // 사용자 ID로 검색
void db_get_all_users();                                    // 모든 사용자 목록 가져오기
void db_get_user_info(const char *user_id);                 // 특정 사용자 정보 가져오기
void db_recent_user(int limit);                             // 최근 접속 사용자 목록

// ===== 대화방 관련 함수 =====
int db_create_room(Room *room);                                      // 새로운 대화방 생성
void db_remove_room(Room *room);                                     // 대화방 삭제
void db_update_room_name(Room *room, const char *new_name);          // 대화방 이름 변경
void db_update_room_manager(Room *room, const char *new_manager_id); // 대화방 관리자 변경
void db_update_room_member_count(Room *room);                        // 대화방 멤버 수 업데이트
void db_add_user_to_room(Room *room, User *user);                    // 대화방에 사용자 추가
void db_remove_user_from_room(Room *room, User *user);               // 대화방에서 사용자 제거
void db_get_room_info(Room *room);                                   // 특정 대화방 정보 가져오기
int db_get_room_by_name(const char *room_name);                      // 대화방 이름으로 검색
//void db_get_room_by_no(unsigned int room_no);                      // 대화방 번호로 검색
unsigned int db_get_max_room_no();                                   // 최대 대화방 번호 가져오기
void db_get_all_rooms();                                             // 모든 대화방 목록 가져오기
//void db_get_room_members(Room *room);                              // 대화방 멤버 목록 가져오기

// ===== 메시지 관련 함수 =====
void db_insert_message(Room *room, User *user, const char *message); // 메시지 추가
int db_remove_message_by_id(Room *room, User *user, int message_id); // 특정 메시지 ID로 삭제
void db_get_room_message(Room *room, User *user);                    // 대화방 메시지 가져오기

#endif // DB_HELPER_H
