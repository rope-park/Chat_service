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
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL); // 외래 키 제약 조건 활성화
    sqlite3_busy_timeout(db, 5000); // 데이터베이스 잠금 대기 시간 설정 (5초)

    // 데이터베이스 테이블 생성
    const char *sql_user_tbl =
        "CREATE TABLE IF NOT EXISTS user ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "sock_no INTEGER UNIQUE, "
        "user_id TEXT NOT NULL, "
        "connected INTEGER, "
        "timestamp DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')));";
    
    const char *sql_room_tbl =
        "CREATE TABLE IF NOT EXISTS room ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER UNIQUE, "
        "room_name TEXT NOT NULL, "
        "manager_id INTEGER, "
        "member_count INTEGER DEFAULT 0, "
        "created_time DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')), "
        "FOREIGN KEY(manager_id) REFERENCES user(id) ON DELETE SET NULL"
        ");";

    const char *sql_message_tbl =
        "CREATE TABLE IF NOT EXISTS message ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER, "
        "sender_id INTEGER, "
        "context TEXT, "
        "timestamp DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')), "
        "FOREIGN KEY(room_no) REFERENCES room(room_no) ON DELETE CASCADE, "
        "FOREIGN KEY(sender_id) REFERENCES user(id) ON DELETE SET NULL"
        ");";

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

// ======== 사용자 관련 함수 ========
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

// 사용자 삭제 함수 - 사용자 정보를 데이터베이스에서 삭제
void db_remove_user(User *user) {
    const char *sql =
        "DELETE FROM user WHERE sock_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user->sock);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove user error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User '%s' removed successfully\n", user->id);
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

// 사용자 연결 상태 업데이트 함수 - 사용자 재접속/종료 시 업데이트
void db_update_user_connect(User *user) {
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

// 모든 사용자 목록 가져오기 함수 - 데이터베이스에서 모든 사용자 정보를 가져옴
void db_get_all_users() {
    const char *sql = 
        "SELECT user_id, connected, timestamp FROM user;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        int connected = sqlite3_column_int(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("User: %s, Connected: %d, Timestamp: %s\n", user_id, connected, timestamp);
    }
    sqlite3_finalize(stmt);
}

// 사용자 정보 가져오기 함수 - 특정 사용자의 정보를 데이터베이스에서 가져옴
void db_get_user_info(User *user) {
    const char *sql = 
        "SELECT user_id, connected, timestamp FROM user WHERE sock_no = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user->sock);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        int connected = sqlite3_column_int(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("User: %s, Connected: %d, Timestamp: %s\n", user_id, connected, timestamp);
    } else {
        fprintf(stderr, "No user found with sock_no %d\n", user->sock);
    }
    sqlite3_finalize(stmt);
}

// 사용자 ID로 검색 함수 - 특정 사용자 ID를 가진 사용자를 데이터베이스에서 검색
void db_get_user_by_id(const char *user_id) {
    const char *sql = 
        "SELECT sock_no, connected, timestamp FROM user WHERE user_id = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int sock_no = sqlite3_column_int(stmt, 0);
        int connected = sqlite3_column_int(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("User: %s, Sock No: %d, Connected: %d, Timestamp: %s\n", user_id, sock_no, connected, timestamp);
    } else {
        fprintf(stderr, "No user found with ID '%s'\n", user_id);
    }
    sqlite3_finalize(stmt);
}

// 소켓 번호로 사용자 검색 함수 - 특정 소켓 번호를 가진 사용자를 데이터베이스에서 검색
void db_get_user_by_sock(int sock) {
    const char *sql = 
        "SELECT user_id, connected, timestamp FROM user WHERE sock_no = ?;";
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, sock);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        int connected = sqlite3_column_int(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("User: %s, Sock No: %d, Connected: %d, Timestamp: %s\n", user_id, sock, connected, timestamp);
    } else {
        fprintf(stderr, "No user found with sock_no %d\n", sock);
    }
    sqlite3_finalize(stmt);
}


// ======== 대화방 관련 함수 ========
// 대화방 생성 함수 - 대화방 정보를 데이터베이스에 삽입
void db_create_room(Room *room) {
    const char *sql =
        "INSERT INTO room (room_no, room_name, manager_id, member_count) "
        "VALUES (?, ?, ?, 1);";

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

// 대화방 삭제 함수 - 대화방 정보를 데이터베이스에서 삭제
void db_remove_room(Room *room) {
    const char *sql =
        "DELETE FROM room WHERE room_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove room error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Room '%s' removed successfully\n", room->room_name);
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

// 대화방 멤버 수 업데이트 함수 - 대화방 참여자 수 업데이트
void db_update_room_member_count(Room *room) {
    const char *sql =
        "UPDATE room SET member_count = ? WHERE room_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->member_count);
    sqlite3_bind_int(stmt, 2, room->no);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update room member count error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Room member count updated successfully\n");
    }
    sqlite3_finalize(stmt);
}

// 대화방에 사용자 추가 함수 - 대화방에 사용자를 추가하고 멤버 수 업데이트
void db_add_user_to_room(Room *room, User *user) {
    const char *sql =
        "INSERT INTO room (room_no, room_name, manager_id, member_count) "
        "VALUES (?, ?, ?, 1);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, room->room_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, user->id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL add user to room error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User '%s' added to room '%s' successfully\n", user->id, room->room_name);
    }
    sqlite3_finalize(stmt);
}

// 대화방에서 사용자 제거 함수 - 대화방에서 사용자를 제거하고 멤버 수 업데이트
void db_remove_user_from_room(Room *room, User *user) {
    const char *sql =
        "DELETE FROM room WHERE room_no = ? AND manager_id = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove user from room error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "User '%s' removed from room '%s' successfully\n", user->id, room->room_name);
    }
    sqlite3_finalize(stmt);
}

// 대화방 정보 가져오기 함수 - 특정 대화방 정보를 데이터베이스에서 가져옴
void db_get_room_info(Room *room) {
    const char *sql =
        "SELECT room_name, manager_id, member_count, created_time FROM room WHERE room_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *room_name = (const char *)sqlite3_column_text(stmt, 0);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 1);
        int member_count = sqlite3_column_int(stmt, 2);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 3);
        printf("Room: %s, Manager: %s, Members: %d, Created: %s\n", room_name, manager_id, member_count, created_time);
    } else {
        fprintf(stderr, "No room found with no %d\n", room->no);
    }
    sqlite3_finalize(stmt);
}

// 대화방 이름으로 검색 함수 - 특정 대화방 이름을 가진 대화방을 데이터베이스에서 검색
void db_get_room_by_name(const char *room_name) {
    const char *sql =
        "SELECT room_no, manager_id, member_count, created_time FROM room WHERE room_name = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int room_no = sqlite3_column_int(stmt, 0);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 1);
        int member_count = sqlite3_column_int(stmt, 2);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 3);
        printf("Room: %s, No: %d, Manager: %s, Members: %d, Created: %s\n", room_name, room_no, manager_id, member_count, created_time);
    } else {
        fprintf(stderr, "No room found with name '%s'\n", room_name);
    }
    sqlite3_finalize(stmt);
}

// 대화방 ID로 검색 함수 - 특정 대화방 번호를 가진 대화방을 데이터베이스에서 검색
void db_get_room_by_no(unsigned int room_no) {
    const char *sql =
        "SELECT room_name, manager_id, member_count, created_time FROM room WHERE room_no = ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room_no);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *room_name = (const char *)sqlite3_column_text(stmt, 0);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 1);
        int member_count = sqlite3_column_int(stmt, 2);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 3);
        printf("Room No: %d, Name: %s, Manager: %s, Members: %d, Created: %s\n", room_no, room_name, manager_id, member_count, created_time);
    } else {
        fprintf(stderr, "No room found with no %d\n", room_no);
    }
    sqlite3_finalize(stmt);
}

// 대화방 목록 가져오기 함수 - 데이터베이스에서 모든 대화방 정보를 가져옴
void db_get_all_rooms() {
    const char *sql =
        "SELECT room_no, room_name, manager_id, member_count, created_time FROM room;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int room_no = sqlite3_column_int(stmt, 0);
        const char *room_name = (const char *)sqlite3_column_text(stmt, 1);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 2);
        int member_count = sqlite3_column_int(stmt, 3);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 4);
        printf("Room No: %d, Name: %s, Manager: %s, Members: %d, Created: %s\n", room_no, room_name, manager_id, member_count, created_time);
    }
    sqlite3_finalize(stmt);
}

// 대화방 참여자 목록 가져오기 함수 - 특정 대화방의 참여자 정보를 데이터베이스에서 가져옴
void db_get_room_members(Room *room) {
    const char *sql =
        "SELECT user_id FROM user WHERE sock_no IN ("
        "SELECT sock_no FROM room WHERE room_no = ?);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        printf("Member: %s\n", user_id);
    }
    sqlite3_finalize(stmt);
}


// ======== 메시지 관련 함수 ========
// 메시지 추가 함수 - 대화방 메시지를 데이터베이스에 삽입
void db_insert_message(Room *room, User *user, const char *message) {
    const char *sql =
        "INSERT INTO message (room_no, sender_id, context) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, message, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert message error: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Message from '%s' in room '%s' inserted successfully\n", user->id, room->room_name);
    }
    sqlite3_finalize(stmt);
}

// // 대화방 메시지 가져오기 함수 - 사용자 재접속 시 이전에 leave하지 않은 대화방의 메시지를 가져옴
// 사용자의 최초 대화방 참여 이후부터 현재까지의 메시지를 가져옴
void db_get_room_message(Room *room, User *user) {

    const char *sql = 
        "SELECT sender_id, context, timestamp FROM message "
        "WHERE room_no = ? ORDER BY timestamp ASC;";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, room->no); 
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sender_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *context = (const char *)sqlite3_column_text(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("[%s] %s: %s\n", timestamp, sender_id, context); // 메시지 출력
    }
    sqlite3_finalize(stmt);
}최근 접속 사용자 목록 함수 - 최근 접속한 사용자 정보를 가져옴


// ========== 최근 접속 사용자 목록 함수 =========
// 최근 접속 사용자 목록 함수 - 최근 접속한 사용자 정보를 데이터베이스에서 가져옴
void db_recent_user(int limit) {
    const char *sql =
        "SELECT user_id, connected, timestamp FROM user "
        "WHERE connected = 1 ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, limit);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        int connected = sqlite3_column_int(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        printf("User: %s, Connected: %d, Timestamp: %s\n", user_id, connected, timestamp);
    }
    sqlite3_finalize(stmt);
}
