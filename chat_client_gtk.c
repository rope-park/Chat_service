#include <gtk/gtk.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>

#define BUFFER_SIZE 2048

// UI 요소
GtkWidget *ip_entry;            // IP 주소 입력 필드
GtkWidget *port_entry;          // 포트 번호 입력 필드
GtkWidget *connect_button;      // 연결 버튼
GtkWidget *chat_view;           // 채팅 뷰 (텍스트 뷰)
GtkWidget *tool_palette;        // 도구 팔레트 (툴바)
GtkWidget *room_create_entry;   // 대화방 생성 입력 필드
GtkWidget *room_create_button;  // 대화방 생성 버튼
GtkWidget *message_entry;       // 메시지 입력 필드
GtkWidget *send_button;         // 메시지 전송 버튼
GtkWidget *window;              // 메인 창
GtkWidget *scrolled_window;     // 스크롤 가능한 윈도우

int sock = -1;
pthread_t recv_thread_id = 0; // 수신 스레드 ID

// 위젯과 민감도 상태 저장 - UI 요소의 민감도를 안전하게 설정하기 위해 사용
typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} WidgetSensitivityData;

// 다른 스레드에서 UI 요소를 안전하게 업데이트하기 위한 idle 콜백
// GTK는 UI 업데이트를 메인 스레드에서만 수행할 수 있으므로, g_idle_add를 사용하여 메인 스레드에서 실행되도록 함
static gboolean append_message_to_view_idle(gpointer user_data) {
    char *message = (char *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat_view)); // 텍스트 뷰의 버퍼 가져오기
    GtkTextIter end_iter; // 텍스트 삽입 위치를 위한 반복자
    gtk_text_buffer_get_end_iter(buffer, &end_iter); // 버퍼의 끝 위치 가져오기
    gtk_text_buffer_insert(buffer, &end_iter, message, -1); // 메시지 삽입
    gtk_text_buffer_insert(buffer, &end_iter, "\n", -1);    // 줄 바꿈 추가

    // 스크롤 바를 항상 아래로 이동
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
    if (vadj) {
        gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
    }
    g_free(message);
    return G_SOURCE_REMOVE;
}

// 다른 스레드에서 위젯의 민감도를 안전하게 설정하기 위한 idle 콜백
static gboolean set_widget_sensitive_idle(gpointer user_data) {
    WidgetSensitivityData *data = (WidgetSensitivityData *)user_data;
    gtk_widget_set_sensitive(data->widget, data->sensitive);
    g_free(data);
    return G_SOURCE_REMOVE;
}

// Send 버튼의 레이블을 "Send"로 설정하는 idle 콜백 - 연결 성공 시 호출됨
static gboolean set_send_button_label_send(gpointer data) {
    (void)data; // 사용하지 않는 인자
    gtk_button_set_label(GTK_BUTTON(send_button), "Send");
    return G_SOURCE_REMOVE;
}

// 서버로부터 메시지를 수신하는 스레드 함수
static void *receive_messages() {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while ((bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        g_idle_add(append_message_to_view_idle, g_strdup(buffer));

        if (strstr(buffer, "[Server] Welcome") != NULL) {
            g_idle_add((GSourceFunc)set_send_button_label_send, NULL);
            gtk_widget_set_sensitive(room_create_button, TRUE);
            gtk_widget_set_sensitive(room_create_entry, TRUE);
        }
    }

    if (bytes_received == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected from server."));
    } else if (bytes_received < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving message."));
    }

    // 연결 종료 시 UI 요소의 민감도 상태 변경
    WidgetSensitivityData *send_data = g_new(WidgetSensitivityData, 1);
    send_data->widget = send_button; send_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, send_data);
    WidgetSensitivityData *msg_data = g_new(WidgetSensitivityData, 1);
    msg_data->widget = message_entry; msg_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, msg_data);
    WidgetSensitivityData *conn_data = g_new(WidgetSensitivityData, 1);
    conn_data->widget = connect_button; conn_data->sensitive = TRUE;
    g_idle_add(set_widget_sensitive_idle, conn_data);

    // receive_messages()에서 연결 종료 감지 시
    gtk_widget_set_sensitive(message_entry, FALSE);
    gtk_widget_set_sensitive(send_button, FALSE);
    gtk_widget_set_sensitive(room_create_entry, FALSE);
    gtk_widget_set_sensitive(room_create_button, FALSE);
    gtk_widget_set_sensitive(connect_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(send_button), "OK");

    if (sock != -1) {
        close(sock);
        sock = -1;
    }
    recv_thread_id = 0;
    return NULL;
}

// Connect 버튼 클릭 시 호출되는 콜백 함수 - 서버에 연결 시도
static void on_connect_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; // 사용하지 않는 인자
    (void)data; // 사용하지 않는 인자

    if (sock != -1) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Already connected or attempting to connect."));
        return;
    }
    const char *ip_address = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    const char *port_str = gtk_entry_get_text(GTK_ENTRY(port_entry));
    int port = atoi(port_str);

    if (strlen(ip_address) == 0 || port <= 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Please enter a valid IP address and port number."));
        return;
    }

    struct sockaddr_in server_addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Socket creation failed."));
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Invalid IP address format."));
        close(sock); sock = -1; return;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Connection failed."));
        close(sock); sock = -1; return;
    }

    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Connected to server!"));
    gtk_widget_set_sensitive(connect_button, FALSE);
    gtk_widget_set_sensitive(send_button, TRUE);
    gtk_widget_set_sensitive(message_entry, TRUE);
    gtk_widget_grab_focus(message_entry);
    gtk_button_set_label(GTK_BUTTON(send_button), "OK");

    if (pthread_create(&recv_thread_id, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create for receive_messages");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Failed to create receive thread."));
        close(sock); sock = -1;
        gtk_widget_set_sensitive(connect_button, TRUE);
        gtk_widget_set_sensitive(send_button, FALSE);
        gtk_widget_set_sensitive(message_entry, FALSE);
    }
}

// Send 버튼 클릭 시 호출되는 콜백 함수 - 메시지 전송
static void on_send_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; // 사용하지 않는 인자
    (void)data; // 사용하지 않는 인자

    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }

    // ID 입력 단계: Send 버튼 라벨이 "OK"인 경우
    const char *btn_label = gtk_button_get_label(GTK_BUTTON(send_button));
    if (strcmp(btn_label, "OK") == 0) {
        const char *id = gtk_entry_get_text(GTK_ENTRY(message_entry));
        if (!id || strlen(id) == 0) {
            g_idle_add(append_message_to_view_idle, g_strdup("[Client] Please enter a valid user ID."));
            return;
        }

        // ID 비어있지 않으면 서버로 전송
        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "%s\n", id);
        if (send(sock, buffer, strlen(buffer), 0) < 0) {
            perror("send");
            g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send user ID."));
        }
        gtk_entry_set_text(GTK_ENTRY(message_entry), ""); // 메시지 입력 필드 비우기
        gtk_widget_grab_focus(message_entry); // 메시지 입력 필드에 포커스 설정
        return;
    }

    // 채팅 메시지 단계
    const char *message = gtk_entry_get_text(GTK_ENTRY(message_entry));
    if (strlen(message) == 0) return;

    char buffer_to_send[BUFFER_SIZE];
    snprintf(buffer_to_send, sizeof(buffer_to_send), "%s\n", message);

    if (send(sock, buffer_to_send, strlen(buffer_to_send), 0) < 0) {
        perror("send");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send message."));
    }
    gtk_entry_set_text(GTK_ENTRY(message_entry), "");
    gtk_widget_grab_focus(message_entry);
}

// 창이 닫힐 때 호출되는 콜백 함수 - 소켓 종료 및 스레드 정리
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget; // 사용하지 않는 인자
    (void)data; // 사용하지 않는 인자

    if (sock != -1) {
        shutdown(sock, SHUT_RDWR); close(sock); sock = -1;
    }
    if (recv_thread_id != 0) { 
        pthread_join(recv_thread_id, NULL);
    }
    gtk_main_quit();
}

// 대화방 생성 버튼 클릭 시 호출되는 콜백 함수 - 대화방 생성 요청
static void on_create_room_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; // 사용하지 않는 인자
    (void)data; // 사용하지 않는 인자

    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    const char *room_name = gtk_entry_get_text(GTK_ENTRY(room_create_entry));
    if (strlen(room_name) == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Please enter a room name."));
        return;
    }

    char buffer_create[BUFFER_SIZE];
    snprintf(buffer_create, sizeof(buffer_create), "/create %s\n", room_name);

    if (send(sock, buffer_create, strlen(buffer_create), 0) < 0) {
        perror("send");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to create room."));
    }
    gtk_entry_set_text(GTK_ENTRY(room_create_entry), "");
    gtk_widget_grab_focus(message_entry);

    gtk_widget_hide(room_create_entry);
    gtk_widget_hide(room_create_button);
}

// 메인 창 활성화 시 호출되는 함수 - UI 초기화 및 설정
static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data; // 사용하지 않는 인자
    
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, "style.css", NULL); // CSS 파일 로드
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
    GtkWidget *grid, *ip_label, *port_label;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 900); 
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL); // 창 닫기 시 콜백

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_container_add(GTK_CONTAINER(window), grid);

    ip_label = gtk_label_new("Server IP:");
    gtk_grid_attach(GTK_GRID(grid), ip_label, 0, 0, 1, 1); // 행 0, 열 0
    ip_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ip_entry), "127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), ip_entry, 1, 0, 1, 1); // 행 0, 열 1

    port_label = gtk_label_new("Port:");
    gtk_grid_attach(GTK_GRID(grid), port_label, 2, 0, 1, 1); // 행 0, 열 2
    port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(port_entry), "9000");
    gtk_grid_attach(GTK_GRID(grid), port_entry, 3, 0, 1, 1); // 행 0, 열 3

    connect_button = gtk_button_new_with_label("Connect");
    gtk_widget_set_name(connect_button, "connect_button");
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), connect_button, 4, 0, 1, 1); // 행 0, 열 4

    scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC); // 스크롤바 자동 표시
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_hexpand(scrolled_window, TRUE);

    chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD_CHAR);

    gtk_widget_set_vexpand(chat_view, TRUE);
    gtk_widget_set_hexpand(chat_view, TRUE);

    gtk_container_add(GTK_CONTAINER(scrolled_window), chat_view);
    gtk_grid_attach(GTK_GRID(grid), scrolled_window, 0, 1, 5, 1); // 행 1, 열 0, 5열 차지

    message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(message_entry, TRUE);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), message_entry, 0, 2, 4, 1); // 행 2, 열 0, 4열 차지
    gtk_widget_set_sensitive(message_entry, FALSE);

    send_button = gtk_button_new_with_label("Send");
    gtk_widget_set_name(send_button, "send_button");
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), send_button, 4, 2, 1, 1); // 행 2, 열 4, 1열 차지
    gtk_widget_set_sensitive(send_button, FALSE);

    room_create_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(room_create_entry), "Enter Room Name");
    gtk_grid_attach(GTK_GRID(grid), room_create_entry, 0, 3, 4, 1); // 행 3, 열 0, 4열 차지
    
    room_create_button = gtk_button_new_with_label("Create Room");
    gtk_grid_attach(GTK_GRID(grid), room_create_button, 4, 3, 1, 1); // 행 3, 열 4, 1열 차지
    g_signal_connect(room_create_button, "clicked", G_CALLBACK(on_create_room_clicked), room_create_entry);

    gtk_widget_show_all(window);
    gtk_widget_hide(room_create_entry);
    gtk_widget_hide(room_create_button);
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("org.chat.gtk", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}