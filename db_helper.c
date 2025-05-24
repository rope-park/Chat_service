#include <stdio.h>
#include <sqlite3.h>
#include "db_helper.h"

sqlite3 *db = NULL;

// 데이터베이스 초기화 함수 - 데이터베이스 파일 열기, 테이블 생성
void db_init() {
    int rc = sqlite3_open("chat.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(1);
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }

    // 데이터베이스 테이블 생성
    const char *sql_user_tbl =
        "CREATE TABLE IF NOT EXISTS user ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "sock_no INTEGER UNIQUE, "
        "user_id TEXT NOT NULL, "
        "connected INTEGER, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";
    
    const char *sql_room_tbl =
        "CREATE TABLE IF NOT EXISTS room ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER UNIQUE, "
        "room_name TEXT NOT NULL, "
        "manager_id TEXT, "
        "member_count INTEGER DEFAULT 0, "
        "created_time DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "FOREIGN KEY(manager_id) REFEREBCES user(id));";

    const char *sql_message_tbl =
        "CREATE TABLE IF NOT EXISTS message ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER, "
        "sender_id TEXT, "
        "context TEXT, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "FOREIGN KEY(room_no) REFERENCES room(room_no), "
        "FOREIGN KEY(user_id) REFERENCES user(user_id));";

    char *err_msg;
    rc = sqlite3_exec(db, sql_user_tbl, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL user_tbl error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        fprintf(stderr, "User table created successfully\n");
    }

    rc = sqlite3_exec(db, sql_room_tbl, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL room_tbl error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        fprintf(stderr, "Room table created successfully\n");
    }

    rc = sqlite3_exec(db, sql_message_tbl, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL message_tbl error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        fprintf(stderr, "Message table created successfully\n");
    }
}

// 데이터베이스 종료 함수 - 데이터베이스 연결 닫기
void db_close() {
    if (db) {
        sqlite3_close(db);
        fprintf(stderr, "Database closed successfully\n");
    }
}

// 사용자 추가 함수 - 사용자 정보를 데이터베이스에 삽입
void db_insert_user(User *user) {
    const char *sql = 
        "INSERT INTO user (sock_no, user_id, connected) VALUES (?, ?, 1);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user->sock);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert user error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User '%s' inserted successfully\n", user->id);
    }
    sqlite3_finalize(stmt);
}
// 사용자 ID 변경 함수 - 사용자 ID 업데이트
void db_update_user_id(User *user, const char *new_id) {
    const char *sql = 
        "UPDATE user SET user_id = ? WHERE sock_no = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, new_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, user->sock);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update user ID error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User ID updated to '%s' successfully\n", new_id);
    }
    sqlite3_finalize(stmt);
}

// 사용자 연결 해제 함수 - 사용자의 연결 상태 업데이트
void db_disconnect_user(User *user) {
    const char *sql =
        "UPDATE user SET connected = 0 WHERE sock_no = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user->sock);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL disconnect user error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User '%s' disconnected successfully\n", user->id);
    }
    sqlite3_finalize(stmt);
}

// 대화방 생성 함수 - 대화방 정보를 데이터베이스에 삽입
void db_create_room(Room *room) {
    const char *sql =
        "INSERT INTO room (room_no, room_name, manager_id, member_count) "
        "VALUES (?, ?, ?, 0);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, room->room_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, room->manager->id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL create room error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Room '%s' created successfully\n", room->room_name);
    }
    sqlite3_finalize(stmt);
}

// 대화방 이름 변경 함수 - 대화방 이름 업데이트
void db_update_room_name(Room *room, const char *new_name) {
    const char *sql =
        "UPDATE room SET room_name = ? WHERE room_no = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, room->no);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update room name error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Room name updated to '%s' successfully\n", new_name);
    }
    sqlite3_finalize(stmt);
}

// 대화방 방장 변경 함수 - 대화방 방장 ID 업데이트
void db_update_room_manager(Room *room, const char *new_manager_id) {
    const char *sql =
        "UPDATE room SET manager_id = ? WHERE room_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, new_manager_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, room->no);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update room manager error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Room manager updated to '%s' successfully\n", new_manager_id);
    }
    sqlite3_finalize(stmt);
}


void db_insert_message(Room *room, User *user, const char *message);