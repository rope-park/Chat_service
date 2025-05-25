#ifndef DB_HELPER_H
#define DB_HELPER_H

#include <sqlite3.h>
#include "chat_server.h"

extern sqlite3 *db;

// 데이터베이스 초기화 및 종료 함수
void db_init();
void db_close();

// 사용자 관련 함수
void db_insert_user(User *user);                            // 사용자 추가
void db_update_user_id(User *user, const char *new_id);     // 사용자 ID 변경
void db_disconnect_user(User *user);                        // 사용자 연결 해제

// 대화방 관련 함수
void db_create_room(Room *room);                                // 대화방 생성
void db_update_room_name(Room *room, const char *new_name);     // 대화방 이름 변경
void db_update_room_manager(Room *room, const char *new_manager_id);     // 대화방 관리자 변경

// 메시지 관련 함수
void db_insert_message(Room *room, User *user, const char *message); // 메시지 추가

// 데이터베이스에서 사용자 검색
void db_recent_user(int limit); // 최근 접속 사용자 목록
void db_get_room_message(Room *room, User *user); // 대화방 메시지 가져오기

#endif // DB_HELPER_H
