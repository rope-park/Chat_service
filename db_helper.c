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
        "sock_no INTEGER NOT NULL, "
        "user_id TEXT UNIQUE NOT NULL, "
        "connected INTEGER, "
        "timestamp DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')));";
    
    const char *sql_room_tbl =
        "CREATE TABLE IF NOT EXISTS room ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER UNIQUE, "
        "room_name TEXT NOT NULL UNIQUE, "
        "manager_id TEXT, "
        "member_count INTEGER DEFAULT 0, "
        "created_time DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')), "
        "FOREIGN KEY(manager_id) REFERENCES user(id));";

    const char *sql_message_tbl =
        "CREATE TABLE IF NOT EXISTS message ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "room_no INTEGER, "
        "sender_id TEXT, "
        "context TEXT, "
        "timestamp DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')), "
        "FOREIGN KEY(room_no) REFERENCES room(room_no) ON DELETE CASCADE, "
        "FOREIGN KEY(sender_id) REFERENCES user(user_id) ON UPDATE CASCADE);";
    
    const char *sql_room_user_tbl =
        "CREATE TABLE IF NOT EXISTS room_user ("
        "room_no INTEGER,"
        "user_id TEXT, "
        "join_time DATETIME DEFAULT (DATETIME('NOW', 'LOCALTIME')), "
        "PRIMARY KEY(room_no, user_id), "
        "FOREIGN KEY(room_no) REFERENCES room(room_no) ON DELETE CASCADE, "
        "FOREIGN KEY(user_id) REFERENCES user(user_id) ON DELETE CASCADE);";

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

    rc = sqlite3_exec(db, sql_room_user_tbl, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL room_user_tbl error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        fprintf(stderr, "Room_User table created successfully\n");
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
    if (!user || user->id[0] == '\0') return;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "INSERT INTO user (user_id, sock_no, connected) VALUES (?, ?, 1);";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, user->id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, user->sock);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert user error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User '%s' inserted (sock=%d)\n", user->id, user->sock);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 사용자 삭제 함수 - 사용자 정보를 데이터베이스에서 삭제
void db_remove_user(User *user) {
    if (!user || user->id[0] == '\0') return;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "DELETE FROM user WHERE user_id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, user->id, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove user error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User '%s' removed successfully\n", user->id);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 사용자 ID 변경 함수 - 사용자 ID 업데이트
void db_update_user_id(User *user, const char *new_id) {
    if (!user || user->id[0] == '\0') return;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql = 
        "UPDATE user SET user_id = ? WHERE user_id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, new_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update user_id error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User ID updated: '%s' -> '%s'\n", user->id, new_id);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 사용자 연결 상태 업데이트 함수 - 사용자의 연결 상태 업데이트
void db_update_user_connected(User *user, int status) {
    if (!user || user->sock < 0) return;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "UPDATE user SET connected = ? WHERE sock_no = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int(stmt, 2, user->sock);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update user connected error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User '%s' connected status updated to '%d' successfully\n", user->id, status);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 모든 사용자 목록 가져오기 함수 - 데이터베이스에서 모든 사용자 정보를 가져옴
void db_get_all_users() {
    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT sock_no, user_id, connected, timestamp FROM user;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    
    // 사용자 목록을 순회하며 정보를 출력
    printf("%2s\t%16s\t%8s\t%s\n", "sock_no", "ID", "CONNECTED", "TIMESTAMP");
    printf("=========================================================\n");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int sock_no = sqlite3_column_int(stmt, 0);
        const char *user_id = (const char *)sqlite3_column_text(stmt, 1);
        int connected = sqlite3_column_int(stmt, 2);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 3);
        printf("%2d\t%16s\t%8d\t%s\n", sock_no, user_id, connected, timestamp);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 사용자 정보 가져오기 함수 - 특정 사용자 정보를 데이터베이스에서 가져옴
void db_get_user_info(const char *user_id) {
    if (!user_id || strlen(user_id) == 0) {
        fprintf(stderr, "Invalid user_id\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT sock_no, user_id, connected, timestamp FROM user WHERE user_id = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        fprintf(stderr, "Failed to prepare SQL statement for user info\n");
        return;
    }

    sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int sock_no = sqlite3_column_int(stmt, 0);
        const char *user_id = (const char *)sqlite3_column_text(stmt, 1);
        int connected = sqlite3_column_int(stmt, 2);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 3);
        printf("Sock: %d, User ID: %s, Connected: %d, Timestamp: %s\n", sock_no, user_id, connected, timestamp);
    } else {
        fprintf(stderr, "SQL get user info error: %s\n", sqlite3_errmsg(db));
        /*fprintf(stderr, "[DB] User info retrieved for user_id %d\n", user_id);*/
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 사용자 ID로 검색 함수 - 특정 사용자 ID를 가진 사용자를 데이터베이스에서 검색
int db_check_user_id(const char *user_id) {
    if (!user_id || strlen(user_id) == 0) {
        fprintf(stderr, "Invalid user_id\n");
        return 0; // 유효하지 않은 ID
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql = 
        "SELECT 1 FROM user WHERE user_id = ?;";
    sqlite3_stmt *stmt;
    int rc =  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return 0; // 오류 발생
    }
    sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0; // 존재 여부 반환
    if (exists != SQLITE_DONE && exists != SQLITE_ROW) {
        fprintf(stderr, "SQL check user_id error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User ID '%s' exists: %d\n", user_id, exists == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    return exists;
}

// 사용자 소켓 번호로 검색 함수 - 특정 소켓 번호를 가진 사용자를 데이터베이스에서 검색
int db_get_user_by_sock(int sock) {
    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT 1 FROM user WHERE sock_no = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return 0; // 오류 발생
    }
    sqlite3_bind_int(stmt, 1, sock);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0; // 존재 여부 반환
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    return exists; // 존재 여부 반환
}

// 최근 접속 사용자 목록 가져오기 함수 - 최근 접속한 사용자 목록을 가져옴
void db_recent_user(int limit) {
    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT user_id, sock_no, connected, timestamp FROM user "
        "ORDER BY timestamp DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    
    sqlite3_bind_int(stmt, 1, limit);
    
    printf("%16s\t%2s\t%8s\t%s\n", "ID", "sock_no", "CONNECTED", "TIMESTAMP");
    printf("=========================================================\n");
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        int sock_no = sqlite3_column_int(stmt, 1);
        int connected = sqlite3_column_int(stmt, 2);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 3);
        printf("%16s\t%2d\t%8d\t%s\n", user_id, sock_no, connected, timestamp);
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

}

// ======= 대화방 관련 함수 ========
// 대화방 생성 함수 - 대화방 정보를 데이터베이스에 삽입
void db_create_room(Room *room) {
    if (!room || !room->room_name[0] == '\0' || !room->manager) {
        fprintf(stderr, "Invalid room or manager info\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "INSERT INTO room (room_no, room_name, manager_id, member_count) "
        "VALUES (?, ?, ?, 1);";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, room->room_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, room->manager->id, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL create room error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Room '%s' (room_no=%u) created successfully\n", room->room_name, room->no);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 삭제 함수 - 대화방 정보를 데이터베이스에서 삭제
void db_remove_room(Room *room) {
    if (!room) {
        fprintf(stderr, "Invalid room pointer\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "DELETE FROM room WHERE room_no = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    sqlite3_bind_int(stmt, 1, room->no);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove room error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Room '%s' (no=%u) removed from DB.\n", room->room_name, room->no);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 이름 변경 함수 - 대화방 이름 업데이트
void db_update_room_name(Room *room, const char *new_name) {
    if (!room || !new_name || strlen(new_name) == 0) {
        fprintf(stderr, "Invalid room or new name\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "UPDATE room SET room_name = ? WHERE room_no = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, room->no);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update room name error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Room name updated to '%s' (room_no=%u) successfully\n", new_name, room->no);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 방장 변경 함수 - 대화방 방장 ID 업데이트
void db_update_room_manager(Room *room, const char *new_manager_id) {
    if (!room || !new_manager_id || strlen(new_manager_id) == 0) {
        fprintf(stderr, "Invalid room or new manager ID\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "UPDATE room SET manager_id = ? WHERE room_no = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, new_manager_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, room->no);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "SQL update room manager error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Room manager updated to '%s' successfully\n", new_manager_id);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 멤버 수 업데이트 함수 - 대화방 참여자 수 업데이트
void db_update_room_member_count(Room *room) {
    if (!room) {
        fprintf(stderr, "Invalid room pointer\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "UPDATE room SET member_count = ? WHERE room_no = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    // 멤버 수가 음수인 경우 0으로 설정
    int count = (room->member_count < 0) ? 0 : room->member_count;
    sqlite3_bind_int(stmt, 1, count);
    sqlite3_bind_int(stmt, 2, room->no);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL update room member count error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Room '%s' (room_no=%u) member_count updated to %d\n", room->room_name, room->no, count);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방에 사용자 추가 함수 - 대화방에 사용자를 추가
void db_add_user_to_room(Room *room, User *user) {
    if (!room || !user || !user->id[0] == '\0') {
        fprintf(stderr, "Invalid room or user info\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "INSERT OR IGNORE INTO room_user (room_no, user_id, join_time) VALUES (?, ?, DATETIME('NOW', 'LOCALTIME'));";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL add user to room error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User '%s' added to room '%s' successfully\n", user->id, room->room_name);
        db_update_room_member_count(room); // 멤버 수 업데이트
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방에서 사용자 제거 함수 - 대화방에서 사용자를 제거
void db_remove_user_from_room(Room *room, User *user) {
    if (!room || !user || !user->id[0] == '\0') {
        fprintf(stderr, "Invalid room or user info\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "DELETE FROM room_user WHERE room_no = ? AND user_id = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove user from room error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] User '%s' removed from room '%s' successfully\n", user->id, room->room_name);
        db_update_room_member_count(room); // 멤버 수 업데이트
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 정보 가져오기 함수 - 대화방 정보를 데이터베이스에서 가져옴
void db_get_room_info(Room *room){
    if (!room) {
        fprintf(stderr, "Invalid room pointer\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT room_no, room_name, manager_id, member_count, created_time "
        "FROM room WHERE room_no = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        room->no = sqlite3_column_int(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        strncpy(room->room_name, name ? name : "", sizeof(room->room_name) - 1);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 2);
        strncpy(room->manager->id, manager_id ? manager_id : "", sizeof(room->manager->id) - 1);
        room->member_count = sqlite3_column_int(stmt, 3);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 4);
        printf("[DB] Room Info: No=%u, Name='%s', Manager='%s', Members=%d, Created='%s'\n",
               room->no, room->room_name, room->manager->id, room->member_count, created_time);
    } else {
        fprintf(stderr, "SQL get room info error: %s\n", sqlite3_errmsg(db));
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 이름으로 검색 함수 - 특정 대화방 이름을 가진 대화방을 데이터베이스에서 검색
void db_get_room_by_name(const char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        fprintf(stderr, "Invalid room_name\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT room_no, room_name, manager_id, member_count, created_time "
        "FROM room WHERE room_name = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        unsigned int room_no = sqlite3_column_int(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 2);
        int member_count = sqlite3_column_int(stmt, 3);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 4);
        
        printf("[DB] Room Info: No=%u, Name='%s', Manager='%s', Members=%d, Created='%s'\n",
               room_no, name ? name : "", manager_id ? manager_id : "", member_count, created_time);
    } else {
        fprintf(stderr, "SQL get room by name error: %s\n", sqlite3_errmsg(db));
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 번호로 검색 함수 - 특정 대화방 번호를 가진 대화방을 데이터베이스에서 검색
void db_get_room_by_no(unsigned int room_no) {
    if (room_no == 0) {
        fprintf(stderr, "Invalid room_no\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT room_no, room_name, manager_id, member_count, created_time "
        "FROM room WHERE room_no = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room_no);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 2);
        int member_count = sqlite3_column_int(stmt, 3);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 4);
        
        printf("[DB] Room Info: No=%u, Name='%s', Manager='%s', Members=%d, Created='%s'\n",
               room_no, name ? name : "", manager_id ? manager_id : "", member_count, created_time);
    } else {
        fprintf(stderr, "SQL get room by no error: %s\n", sqlite3_errmsg(db));
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 모든 대화방 목록 가져오기 함수 - 데이터베이스에서 모든 대화방 정보를 가져옴
void db_get_all_rooms() {
    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT room_no, room_name, manager_id, member_count, created_time "
        "FROM room;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    printf("%4s%20s\t%12s%8s\t%s\n", "ROOM ID", "ROOM NAME", "CREATED TIME", "#USER", "MEMBER");
    printf("==============================================================================================================\n");

    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        unsigned int room_no = sqlite3_column_int(stmt, 0);
        const char *room_name = (const char *)sqlite3_column_text(stmt, 1);
        const char *manager_id = (const char *)sqlite3_column_text(stmt, 2);
        int member_count = sqlite3_column_int(stmt, 3);
        const char *created_time = (const char *)sqlite3_column_text(stmt, 4);
        
        printf("%4u%20s\t%12s%8d\t%s\n",
               room_no, room_name ? room_name : "", manager_id ? manager_id : "", member_count, created_time);
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 멤버 목록 가져오기 함수 - 특정 대화방의 멤버 정보를 데이터베이스에서 가져옴
void db_get_room_members(Room *room) {
    if (!room) {
        fprintf(stderr, "Invalid room pointer\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT user_id FROM room_user WHERE room_no = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    
    printf("Members in room '%s':\n", room->room_name);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        printf("- %s\n", user_id ? user_id : "Unknown");
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// ======== 메시지 관련 함수들 ========
// 메시지 추가 함수 - 대화방에 메시지를 추가
void db_insert_message(Room *room, User *user, const char *message) {
    if (!room || !user || !message || strlen(message) == 0) {
        fprintf(stderr, "Invalid room, user or message\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "INSERT INTO message (room_no, sender_id, context) VALUES (?, ?, ?);";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, message, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL insert message error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Message from '%s' in room '%s' added successfully\n", user->id, room->room_name);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 메시지 삭제 함수 - 대화방에서 특정 메시지를 삭제
void db_remove_message(Room *room, User *user, const char *message) {
    if (!room || !user || !message || strlen(message) == 0) {
        fprintf(stderr, "Invalid room, user or message\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "DELETE FROM message WHERE room_no = ? AND sender_id = ? AND context = ?;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    sqlite3_bind_int(stmt, 1, room->no);
    sqlite3_bind_text(stmt, 2, user->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, message, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL remove message error: %s\n", sqlite3_errmsg(db));
    } else {
        printf("[DB] Message from '%s' in room '%s' removed successfully\n", user->id, room->room_name);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
}

// 대화방 메시지 가져오기 함수 - 특정 대화방의 메시지를 데이터베이스에서 가져옴
void db_get_room_message(Room *room, User *user) {
    if (!room || !user) {
        fprintf(stderr, "Invalid room or user pointer\n");
        return;
    }

    pthread_mutex_lock(&g_db_mutex);

    // 1. 사용자의 해당 방 최초 입장 시각 조회
    const char *sql_first =
        "SELECT MIN(join_time) FROM room_user WHERE user_id = ? AND room_no = ?;";
    sqlite3_stmt *stmt_first;
    int rc = sqlite3_prepare_v2(db, sql_first, -1, &stmt_first, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error (first join): %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    sqlite3_bind_text(stmt_first, 1, user->id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_first, 2, room->no);

    const char *first_join_time = NULL;
    if (sqlite3_step(stmt_first) == SQLITE_ROW) {
        first_join_time = (const char *)sqlite3_column_text(stmt_first, 0);
    }
    sqlite3_finalize(stmt_first);

    if (!first_join_time) {
        // 입장 기록이 없으면 메시지 없음
        safe_send(user->sock, "[Server] No chat history found for you in this room.\n");
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    // 2. 최초 입장 시각 이후의 메시지 조회
    const char *sql_msg =
        "SELECT sender_id, context, timestamp FROM message "
        "WHERE room_no = ? AND timestamp >= ? ORDER BY timestamp ASC;";
    sqlite3_stmt *stmt_msg;
    rc = sqlite3_prepare_v2(db, sql_msg, -1, &stmt_msg, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL prepare error (msg): %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    sqlite3_bind_int(stmt_msg, 1, room->no);
    sqlite3_bind_text(stmt_msg, 2, first_join_time, -1, SQLITE_STATIC);

    char msg_buf[BUFFER_SIZE];
    int found = 0;
    while (sqlite3_step(stmt_msg) == SQLITE_ROW) {
        const char *sender_id = (const char *)sqlite3_column_text(stmt_msg, 0);
        const char *context = (const char *)sqlite3_column_text(stmt_msg, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt_msg, 2);
        snprintf(msg_buf, sizeof(msg_buf), "[%s] %s: %s\n",
                 timestamp ? timestamp : "Unknown", sender_id ? sender_id : "Unknown", context ? context : "");
        safe_send(user->sock, msg_buf);
        found = 1;
    }
    if (!found) {
        safe_send(user->sock, "[Server] No chat history found for you in this room.\n");
    }

    sqlite3_finalize(stmt_msg);
    pthread_mutex_unlock(&g_db_mutex);
}
