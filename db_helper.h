#ifndef DB_HELPER_H
#define DB_HELPER_H

#include <sqlite3.h>
#include "chat_server.h"

extern sqlite3 *db;

// 데이터베이스 초기화 및 종료 함수
void db_init();
void db_close();

// 사용자 관련 함수
void db_insert_user(User *user);                            // 사용자 생성
void db_remove_user(User *user);                            // 사용자 계정 삭제
void db_update_user_id(User *user, const char *new_id);     // 사용자 ID 변경
void db_update_user_connected(User *user, int status);      // 사용자 연결 상태 업데이트
void db_get_all_users();                                    // 모든 사용자 목록 가져오기
void db_get_user_info(const char *user_id);                 // 사용자 정보 가져오기
int db_check_user_id(const char *user_id);                  // 사용자 ID로 검색
int db_get_user_by_sock(int sock);                          // 소켓 번호로 사용자 검색
void db_recent_user(int limit);                             // 최근 접속 사용자 목록

// 대화방 관련 함수
void db_create_room(Room *room);                                     // 대화방 생성
void db_remove_room(Room *room);                                     // 대화방 삭제
void db_update_room_name(Room *room, const char *new_name);          // 대화방 이름 변경
void db_update_room_manager(Room *room, const char *new_manager_id); // 대화방 관리자 변경
void db_update_room_member_count(Room *room);                        // 대화방 멤버 수 업데이트
void db_add_user_to_room(Room *room, User *user);                    // 대화방에 사용자 추가
void db_remove_user_from_room(Room *room, User *user);               // 대화방에서 사용자 제거
void db_get_room_info(Room *room);                                   // 대화방 정보 가져오기
void db_get_room_by_name(const char *room_name);                     // 대화방 이름으로 검색
void db_get_room_by_no(unsigned int room_no);                        // 대화방 번호로 검색
void db_get_all_rooms();                                             // 모든 대화방 목록 가져오기
void db_get_room_members(Room *room);                                // 대화방 멤버 목록 가져오기

// 메시지 관련 함수
void db_insert_message(Room *room, User *user, const char *message); // 메시지 추가
void db_remove_message(Room *room, User *user, const char *message); // 메시지 삭제
int db_remove_message_by_id(Room *room, User *user, int message_id);// 메시지 ID로 삭제
void db_get_room_message(Room *room, User *user);                    // 대화방 메시지 가져오기

#endif // DB_HELPER_H
