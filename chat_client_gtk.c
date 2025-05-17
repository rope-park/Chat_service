#include <gtk/gtk.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type

#define BUFFER_SIZE 1024

// UI elements
GtkWidget *ip_entry;
GtkWidget *port_entry;
GtkWidget *connect_button;
GtkWidget *chat_view;
GtkWidget *message_entry;
GtkWidget *send_button;
GtkWidget *window; // Main window
GtkWidget *scrolled_window; // Declare scrolled_window globally

int sock = -1; // Socket descriptor for the server connection
pthread_t recv_thread_id = 0; // Thread ID for the receiver thread

// Structure to pass widget and state to idle callback
typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} WidgetSensitivityData;

// Idle callback to safely append message to chat view from another thread
// Returns G_SOURCE_REMOVE to be called only once
// user_data is a duplicated string (g_strdup)
static gboolean append_message_to_view_idle(gpointer user_data) {
    char *message = (char *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat_view));
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(buffer, &end_iter);
    gtk_text_buffer_insert(buffer, &end_iter, message, -1);
    gtk_text_buffer_insert(buffer, &end_iter, "\n", -1);

    // Auto-scroll to the end of the text view
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
    if (vadj) {
        gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
    }
    g_free(message); // Free the string allocated by g_strdup
    return G_SOURCE_REMOVE; // Remove source after execution
}

// Idle callback to safely set widget sensitivity from another thread
// Returns G_SOURCE_REMOVE to be called only once
// user_data is a pointer to WidgetSensitivityData (g_new)
static gboolean set_widget_sensitive_idle(gpointer user_data) {
    WidgetSensitivityData *data = (WidgetSensitivityData *)user_data;
    gtk_widget_set_sensitive(data->widget, data->sensitive);
    g_free(data); // Free the allocated data structure
    return G_SOURCE_REMOVE;
}

// Thread function to receive messages from the server
static void *receive_messages(void *arg) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while ((bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        g_idle_add(append_message_to_view_idle, g_strdup(buffer)); // Use g_idle_add for thread safety
    }

    if (bytes_received == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected from server."));
    } else if (bytes_received < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving message."));
    }

    // Safely update UI elements (disable sending, enable connect)
    WidgetSensitivityData *send_data = g_new(WidgetSensitivityData, 1);
    send_data->widget = send_button; send_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, send_data);
    WidgetSensitivityData *msg_data = g_new(WidgetSensitivityData, 1);
    msg_data->widget = message_entry; msg_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, msg_data);
    WidgetSensitivityData *conn_data = g_new(WidgetSensitivityData, 1);
    conn_data->widget = connect_button; conn_data->sensitive = TRUE;
    g_idle_add(set_widget_sensitive_idle, conn_data);

    if (sock != -1) {
        close(sock);
        sock = -1;
    }
    recv_thread_id = 0; // Mark thread as finished
    return NULL;
}

// Callback for the "Connect" button
static void on_connect_clicked(GtkWidget *widget, gpointer data) {
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
        close(sock); sock = -1; return; // Close socket on connection failure
    }

    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Connected to server!"));
    gtk_widget_set_sensitive(connect_button, FALSE);
    gtk_widget_set_sensitive(send_button, TRUE);
    gtk_widget_set_sensitive(message_entry, TRUE);
    gtk_widget_grab_focus(message_entry);

    if (pthread_create(&recv_thread_id, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create for receive_messages");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Failed to create receive thread."));
        close(sock); sock = -1;
        gtk_widget_set_sensitive(connect_button, TRUE);
        gtk_widget_set_sensitive(send_button, FALSE);
        gtk_widget_set_sensitive(message_entry, FALSE);
    }
}

// Callback for the "Send" button or pressing Enter in the message entry
static void on_send_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
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

// Window destroy callback (cleanup)
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (sock != -1) {
        shutdown(sock, SHUT_RDWR); close(sock); sock = -1;
    }
    if (recv_thread_id != 0) { // If thread was created and potentially running
        pthread_join(recv_thread_id, NULL); // Wait for the receive thread to finish
    }
    gtk_main_quit(); // Quit GTK main loop
}

// Application activate callback (builds UI)
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *grid, *ip_label, *port_label; // Ensure scrolled_window is NOT declared locally here

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL); // Connect cleanup on window close

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_container_add(GTK_CONTAINER(window), grid);

    ip_label = gtk_label_new("Server IP:");
    gtk_grid_attach(GTK_GRID(grid), ip_label, 0, 0, 1, 1); // Row 0, Col 0
    ip_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ip_entry), "127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), ip_entry, 1, 0, 1, 1); // Row 0, Col 1

    port_label = gtk_label_new("Port:");
    gtk_grid_attach(GTK_GRID(grid), port_label, 2, 0, 1, 1); // Row 0, Col 2
    port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(port_entry), "9000");
    gtk_grid_attach(GTK_GRID(grid), port_entry, 3, 0, 1, 1); // Row 0, Col 3

    connect_button = gtk_button_new_with_label("Connect");
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), connect_button, 4, 0, 1, 1); // Row 0, Col 4

    scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC); // Add scrollbars
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled_window), chat_view);
    gtk_grid_attach(GTK_GRID(grid), scrolled_window, 0, 1, 5, 1); // Row 1, Col 0, spans 5 columns

    message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(message_entry, TRUE);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), message_entry, 0, 2, 4, 1); // Row 2, Col 0, spans 4 columns
    gtk_widget_set_sensitive(message_entry, FALSE);

    send_button = gtk_button_new_with_label("Send");
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), send_button, 4, 2, 1, 1); // Row 2, Col 4
    gtk_widget_set_sensitive(send_button, FALSE);

    gtk_widget_show_all(window);
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("org.example.chatclient", G_APPLICATION_NON_UNIQUE); // Use recommended flag
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}