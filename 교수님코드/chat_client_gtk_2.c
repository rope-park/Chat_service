#include <gtk/gtk.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type

// Protocol definitions (mirrored from server)
#define REQ_MAGIC 0x5a5a
#define RES_MAGIC 0xa5a5

typedef enum {
    PACKET_TYPE_MESSAGE = 0,
    PACKET_TYPE_SET_NICKNAME = 1,
    PACKET_TYPE_CREATE_ROOM = 2,
    PACKET_TYPE_JOIN_ROOM = 3,
    PACKET_TYPE_LEAVE_ROOM = 4,
    PACKET_TYPE_LIST_ROOMS = 5,
    PACKET_TYPE_LIST_USERS = 6,
} PacketType;

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint8_t type;
    uint16_t data_len;
} PacketHeader;

typedef struct { // For receiving room list
    uint32_t id;
    char name[64];
    uint16_t member_count;
} SerializableRoomInfo;
#pragma pack(pop)

#define HEADER_SIZE sizeof(PacketHeader)
#define BUFFER_SIZE 1024

// UI elements
GtkWidget *ip_entry;
GtkWidget *port_entry;
GtkWidget *connect_button;
GtkWidget *chat_view;
GtkWidget *message_entry;
GtkWidget *send_button;
GtkWidget *room_name_entry;
GtkWidget *create_room_button;
GtkWidget *list_rooms_button;
GtkWidget *leave_room_button; // Leave chat room button
GtkWidget *nickname_entry;
GtkWidget *set_nickname_button;
GtkWidget *current_nickname_display_label; // Current nickname display label
GtkWidget *room_list_dialog = NULL; 
GtkWidget *user_list_tree_view; // TreeView for user list in the main window panel
GtkListStore *user_list_store = NULL; // List store for user list
unsigned int current_room_id_for_user_list = 0; // To refresh the correct user list
GtkListStore *room_list_store = NULL; // List store for the room TreeView
GtkWidget *window; // Main window
GtkWidget *scrolled_window; // Declare scrolled_window globally

int sock = -1; // Socket descriptor for the server connection
pthread_t recv_thread_id = 0; // Thread ID for the receiver thread

unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length);
ssize_t send_client_packet(int sockfd, uint8_t type, const void *data, uint16_t data_len);
ssize_t recv_all(int sock, void *buf, size_t len);

// Structure to pass widget and state to idle callback
typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} WidgetSensitivityData;

// Structure to pass room list data to idle callback
typedef struct {
    uint16_t count;
    SerializableRoomInfo *rooms; // Dynamically allocated array, needs to be freed
} RoomListData;

// Structure to pass user list data (simple string of names)
typedef struct {
    char *names_data; // Concatenated null-separated names data block
    size_t data_length; // Length of the names_data block
} UserListData;

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

// Idle callback to safely update window title
static gboolean update_window_title_idle(gpointer user_data) {
    char *new_title = (char *)user_data;
    if (window && GTK_IS_WINDOW(window)) { // Check if window exists and is a valid window
        gtk_window_set_title(GTK_WINDOW(window), new_title);
    }
    g_free(new_title); // Free the string allocated by g_strdup or g_strdup_printf
    return G_SOURCE_REMOVE;
}

// Idle callback to update the current nickname display label
static gboolean update_nickname_display_label_idle(gpointer user_data) {
    char *new_label_text = (char *)user_data;
    if (current_nickname_display_label) { // Check if label exists
        gtk_label_set_text(GTK_LABEL(current_nickname_display_label), new_label_text);
    }
    g_free(new_label_text); // Free the g_strdup'd string
    return G_SOURCE_REMOVE;
}

// --- Room List Dialog Functions ---
enum {
    ROOM_LIST_COL_ID = 0,
    ROOM_LIST_COL_NAME,
    ROOM_LIST_COL_MEMBERS,
    ROOM_LIST_NUM_COLS
};

static void on_room_list_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data);
static void populate_room_list_store(RoomListData *data);

static void update_join_button_sensitivity(GtkTreeSelection *selection, gpointer user_data) {
    GtkWidget *join_button = GTK_WIDGET(user_data);
    gtk_widget_set_sensitive(join_button, gtk_tree_selection_get_selected(selection, NULL, NULL));
}

static void on_room_list_dialog_destroy(GtkWidget *widget, gpointer data) {
    if (room_list_dialog == widget) { // Ensure we are nullifying the correct dialog
        room_list_dialog = NULL;
    }
}

static void create_room_list_dialog() {
    if (room_list_dialog) { // If dialog already exists, just present it
        gtk_window_present(GTK_WINDOW(room_list_dialog));
        return;
    }

    room_list_dialog = gtk_dialog_new_with_buttons("Available Rooms",
                                                 GTK_WINDOW(window), // Parent
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 NULL); // No default buttons initially

    // Add custom buttons: Join, Refresh and Close
    gtk_dialog_add_button(GTK_DIALOG(room_list_dialog), "_Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_add_button(GTK_DIALOG(room_list_dialog), "_Refresh", GTK_RESPONSE_APPLY); // Using APPLY for refresh
    GtkWidget* join_button = gtk_dialog_add_button(GTK_DIALOG(room_list_dialog), "_Join Room", GTK_RESPONSE_ACCEPT); // ACCEPT for Join
    gtk_widget_set_sensitive(join_button, FALSE); // Initially disabled

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(room_list_dialog));
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled, 350, 250); // Adjusted size

    // Create the list store (model for the tree view)
    room_list_store = gtk_list_store_new(ROOM_LIST_NUM_COLS, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_UINT);

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(room_list_store));
    g_object_unref(room_list_store); // TreeView now owns a reference, so unref the initial one

    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

    // Column for Room ID
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("ID", renderer, "text", ROOM_LIST_COL_ID, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    // Column for Room Name
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Name", renderer, "text", ROOM_LIST_COL_NAME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    // Column for Member Count
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Members", renderer, "text", ROOM_LIST_COL_MEMBERS, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), tree_view);
    gtk_box_pack_start(GTK_BOX(content_area), scrolled, TRUE, TRUE, 0);

    // Enable Join button when a row is selected
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    g_signal_connect(selection, "changed", G_CALLBACK(update_join_button_sensitivity), join_button);

    // Store tree_view in dialog for easy access in populate_room_list_store
    g_object_set_data(G_OBJECT(room_list_dialog), "tree_view", tree_view);

    g_signal_connect(GTK_DIALOG(room_list_dialog), "response", G_CALLBACK(on_room_list_dialog_response), NULL);
    // Connect the "destroy" signal to our custom handler
    g_signal_connect(room_list_dialog, "destroy", G_CALLBACK(on_room_list_dialog_destroy), NULL);

    gtk_widget_show_all(room_list_dialog);
}

static void populate_room_list_store(RoomListData *data) {
    if (!room_list_dialog || !GTK_IS_WIDGET(room_list_dialog)) return; // Dialog not visible or destroyed
    GtkWidget* tree_view = GTK_WIDGET(g_object_get_data(G_OBJECT(room_list_dialog), "tree_view"));
    if (!tree_view) return;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)));
    if (!store) return;

    gtk_list_store_clear(store); // Clear previous entries
    GtkTreeIter tree_iter;

    for (uint16_t i = 0; i < data->count; ++i) {
        gtk_list_store_append(store, &tree_iter);
        gtk_list_store_set(store, &tree_iter,
                           ROOM_LIST_COL_ID, data->rooms[i].id,
                           ROOM_LIST_COL_NAME, data->rooms[i].name,
                           ROOM_LIST_COL_MEMBERS, data->rooms[i].member_count,
                           -1);
    }
}

// Idle callback to create/update room list dialog
static gboolean show_or_update_room_list_idle(gpointer user_data) {
    RoomListData *data = (RoomListData *)user_data;
    // If dialog doesn't exist or was destroyed, create it
    if (!room_list_dialog || !GTK_IS_WIDGET(room_list_dialog) || !gtk_widget_get_visible(room_list_dialog)) {
        create_room_list_dialog(); // Creates and shows
    }
    // Populate the store with new data
    populate_room_list_store(data);

    // Ensure the dialog is presented (brought to front) if it was already open but hidden
    if (room_list_dialog && GTK_IS_WIDGET(room_list_dialog)) {
         gtk_window_present(GTK_WINDOW(room_list_dialog));
    }

    if (data->rooms) free(data->rooms); // Free the array of rooms
    g_free(data); // Free the container
    return G_SOURCE_REMOVE;
}

// --- User List Dialog Functions ---
enum {
    USER_LIST_COL_NICKNAME = 0,
    USER_LIST_NUM_COLS_MAIN_PANEL // Renamed to avoid conflict if dialog version is ever re-added
};

// Function to populate the user list store for the main window panel
static void populate_main_user_list_store(UserListData *data) {
    if (!user_list_tree_view || !GTK_IS_WIDGET(user_list_tree_view)) return;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(user_list_tree_view)));
    if (!store) return;

    gtk_list_store_clear(store); // Clear previous entries
    GtkTreeIter tree_iter;

    if (data->names_data && data->data_length > 0) {
        const char *ptr = data->names_data;
        const char *end_ptr = data->names_data + data->data_length;

        while (ptr < end_ptr) {
            size_t current_name_len = strlen(ptr); // Each segment is null-terminated

            if (current_name_len > 0) { // Add non-empty names
                gtk_list_store_append(store, &tree_iter);
                gtk_list_store_set(store, &tree_iter, USER_LIST_COL_NICKNAME, ptr, -1);
            }

            // Move to the start of the next string (after the current string and its null terminator)
            ptr += current_name_len + 1;

            // The loop condition `while (ptr < end_ptr)` will handle termination.
            // If current_name_len was 0 (empty string segment), ptr advanced by 1.
            // If ptr is still less than end_ptr, the loop continues.
            // This correctly handles sequences like "name1\0name2\0" and also "name1\0\0name2\0"
            // if data_length is accurate.
        }
    }
}

// Idle callback to update the main user list panel
static gboolean show_or_update_main_user_list_idle(gpointer user_data) {
    UserListData *data = (UserListData *)user_data;
    populate_main_user_list_store(data);
    if (data->names_data) g_free(data->names_data); // Free the data block
    g_free(data); // Free the container
    return G_SOURCE_REMOVE;
}

// Idle callback to clear the nickname entry field
static gboolean clear_nickname_entry_idle(gpointer user_data) {
    if (nickname_entry) {
        gtk_entry_set_text(GTK_ENTRY(nickname_entry), "");
    }
    return G_SOURCE_REMOVE;
}

// Helper function to request user list for the current room
static void request_current_room_user_list() {
    if (sock >= 0 && current_room_id_for_user_list > 0) {
        send_client_packet(sock, PACKET_TYPE_LIST_USERS, &current_room_id_for_user_list, sizeof(unsigned int));
    } else if (user_list_tree_view) { // If not in a room, clear the list
        GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(user_list_tree_view)));
        if (store) {
            gtk_list_store_clear(store);
        }
    }
}

unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length) {
    unsigned char cs = 0;
    for (size_t i = 0; i < length; i++) {
        cs ^= header_and_data[i];
    }
    return cs;
}

ssize_t recv_all(int sock_fd, void *buf, size_t len) {
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t received = recv(sock_fd, (char*)buf + total_received, len - total_received, 0);
        if (received <= 0) {
            return received;
        }
        total_received += received;
    }
    return total_received;
}

// Thread function to receive messages from the server
static void *receive_messages(void *arg) {
    PacketHeader pk_header;
    unsigned char *data_buffer = NULL;
    unsigned char *full_payload_buffer = NULL; // For checksum calculation

    while (sock != -1) {
        ssize_t header_bytes = recv_all(sock, &pk_header, HEADER_SIZE);
        if (header_bytes <= 0) {
            if (header_bytes == 0) g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected (header read)."));
            else g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving header."));
            break;
        }

        pk_header.magic = ntohs(pk_header.magic);
        pk_header.data_len = ntohs(pk_header.data_len);

        if (pk_header.magic != RES_MAGIC) {
            g_idle_add(append_message_to_view_idle, g_strdup("[Client] Received bad magic from server."));
            break;
        }
        if (pk_header.data_len > BUFFER_SIZE * 4) { // Increased client buffer for lists
             g_idle_add(append_message_to_view_idle, g_strdup("[Client] Received too large data_len from server."));
             break;
        }

        size_t payload_size = HEADER_SIZE + pk_header.data_len;
        full_payload_buffer = malloc(payload_size);
        if (!full_payload_buffer) { g_idle_add(append_message_to_view_idle, g_strdup("[Client] Malloc failed for payload.")); break; }

        PacketHeader temp_header_for_cs; // For checksum calculation
        temp_header_for_cs.magic = htons(RES_MAGIC);
        temp_header_for_cs.type = pk_header.type;
        temp_header_for_cs.data_len = htons(pk_header.data_len);
        memcpy(full_payload_buffer, &temp_header_for_cs, HEADER_SIZE);

        if (pk_header.data_len > 0) {
            data_buffer = malloc(pk_header.data_len);
            if (!data_buffer) { g_idle_add(append_message_to_view_idle, g_strdup("[Client] Malloc failed for data.")); free(full_payload_buffer); break; }
            ssize_t data_bytes = recv_all(sock, data_buffer, pk_header.data_len);
            if (data_bytes <= 0) { g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving data.")); free(data_buffer); free(full_payload_buffer); break; }
            memcpy(full_payload_buffer + HEADER_SIZE, data_buffer, pk_header.data_len);
        } else {
            data_buffer = NULL;
        }

        unsigned char received_checksum;
        ssize_t checksum_byte = recv_all(sock, &received_checksum, 1);
        if (checksum_byte <= 0) { g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error receiving checksum.")); if(data_buffer) free(data_buffer); free(full_payload_buffer); break; }

        unsigned char calculated_cs = calculate_checksum(full_payload_buffer, payload_size);
        free(full_payload_buffer); full_payload_buffer = NULL;

        if (calculated_cs != received_checksum) {
            g_idle_add(append_message_to_view_idle, g_strdup("[Client] Checksum mismatch from server."));
            if(data_buffer) free(data_buffer);
            continue;
        }

        if (pk_header.type == PACKET_TYPE_LIST_ROOMS) {
            if (data_buffer && pk_header.data_len >= sizeof(uint16_t)) { // Must have at least room count
                RoomListData *list_data = g_new(RoomListData, 1); // Allocate our data structure
                uint16_t room_count_net;
                memcpy(&room_count_net, data_buffer, sizeof(uint16_t));
                list_data->count = ntohs(room_count_net);
                size_t expected_data_size = sizeof(uint16_t) + list_data->count * sizeof(SerializableRoomInfo);
                if (pk_header.data_len == expected_data_size && list_data->count > 0) {
                    list_data->rooms = g_new(SerializableRoomInfo, list_data->count); // Allocate array for rooms
                    SerializableRoomInfo *sri_array_net = (SerializableRoomInfo*)(data_buffer + sizeof(uint16_t));
                    for (uint16_t i = 0; i < list_data->count; ++i) {
                        list_data->rooms[i].id = ntohl(sri_array_net[i].id);
                        strncpy(list_data->rooms[i].name, sri_array_net[i].name, sizeof(list_data->rooms[i].name) - 1);
                        list_data->rooms[i].name[sizeof(list_data->rooms[i].name) - 1] = '\0';
                        list_data->rooms[i].member_count = ntohs(sri_array_net[i].member_count);
                    }
                } else if (list_data->count == 0 && pk_header.data_len == sizeof(uint16_t)) { // Correctly handles 0 rooms
                    list_data->rooms = NULL; // No rooms to allocate
                } else { // Malformed data
                    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Received malformed room list data."));
                    if (list_data->rooms) g_free(list_data->rooms);
                    g_free(list_data); list_data = NULL;
                }
                if (list_data) { // If data was valid or zero rooms
                    g_idle_add(show_or_update_room_list_idle, list_data); // Pass to UI thread
                }
            } else if (pk_header.data_len == 0) { // Server sent no data for room list (should send at least count)
                 g_idle_add(append_message_to_view_idle, g_strdup("[Server] No rooms available (empty data packet)."));
            }
            if(data_buffer) { free(data_buffer); data_buffer = NULL; }
        }
        else if (pk_header.type == PACKET_TYPE_LIST_USERS) {
             UserListData *user_list_data = g_new(UserListData, 1);
             // --- DEBUG LOGGING START ---
             g_print("[CLIENT DEBUG] Received PACKET_TYPE_LIST_USERS. Data len from header: %u\n", pk_header.data_len);
             if (data_buffer && pk_header.data_len > 0) {
                 g_print("[CLIENT DEBUG] User list raw data (first %d bytes of %d as hex): ", MIN(pk_header.data_len, 64), pk_header.data_len);
                 for(guint k=0; k < MIN(pk_header.data_len, 64); ++k) g_print("%02x ", data_buffer[k]);
                 g_print("\n");
             }
             // --- DEBUG LOGGING END ---
             if (data_buffer && pk_header.data_len > 0) {
                 user_list_data->names_data = g_malloc(pk_header.data_len);
                 memcpy(user_list_data->names_data, data_buffer, pk_header.data_len);
                 user_list_data->data_length = pk_header.data_len;
             } else {
                 user_list_data->names_data = NULL;
                 user_list_data->data_length = 0;
             }
             g_idle_add(show_or_update_main_user_list_idle, user_list_data);
             if(data_buffer) { free(data_buffer); data_buffer = NULL; }
        }
        // Generic message display for other types or if data is present
        else if (pk_header.type == PACKET_TYPE_SET_NICKNAME && data_buffer && pk_header.data_len > 0) {
            char display_msg[pk_header.data_len + 1];
            memcpy(display_msg, data_buffer, pk_header.data_len);
            display_msg[pk_header.data_len] = '\0';

            char new_nick_to_set[64] = {0};
            bool nick_extracted = false;

            if (strncmp(display_msg, "[Server] Nickname updated to ", strlen("[Server] Nickname updated to ")) == 0) {
                const char *nick_start = display_msg + strlen("[Server] Nickname updated to ");
                const char *period = strrchr(nick_start, '.');
                if (period && period == nick_start + strlen(nick_start) - 1) { // Ensure '.' is the last char
                    size_t len = period - nick_start;
                    if (len > 0 && len < sizeof(new_nick_to_set)) {
                        strncpy(new_nick_to_set, nick_start, len);
                        new_nick_to_set[len] = '\0';
                        nick_extracted = true;
                    }
                }
            } else if (strncmp(display_msg, "[Server]", strlen("[Server]")) != 0 && strlen(display_msg) > 0) {
                if (strlen(display_msg) < sizeof(new_nick_to_set)) {
                    strcpy(new_nick_to_set, display_msg);
                    nick_extracted = true;
                }
            }

            if (nick_extracted) {
                char *label_text = g_strdup_printf("Current Nick: %s", new_nick_to_set);
                g_idle_add(update_nickname_display_label_idle, label_text);
                if (strncmp(display_msg, "[Server] Nickname updated to ", strlen("[Server] Nickname updated to ")) == 0) {
                    g_idle_add(clear_nickname_entry_idle, NULL);
                }
            }
            // Append the original server message (confirmation or error) to chat view
            g_idle_add(append_message_to_view_idle, g_strdup(display_msg));
            if(data_buffer) { free(data_buffer); data_buffer = NULL; }
        }
        else if (data_buffer && pk_header.data_len > 0) {
            char display_msg[pk_header.data_len + 1]; // VLA, ensure data_len is reasonable
            memcpy(display_msg, data_buffer, pk_header.data_len); // Copy data to display_msg first
            display_msg[pk_header.data_len] = '\0'; // Null-terminate display_msg

            // Check if this is a "joined room" success message to trigger user list request
            // AND update window title
            if (pk_header.type == PACKET_TYPE_JOIN_ROOM &&
                strstr(display_msg, "Joined room '") && // More specific check
                strstr(display_msg, "(ID: ")) {
                unsigned int joined_room_id;
                const char *id_marker = strstr(display_msg, "(ID: ");
                const char *room_name_start_marker = strstr(display_msg, "Joined room '");
                char room_name[64] = {0};

                if (room_name_start_marker) {
                    const char* name_actual_start = room_name_start_marker + strlen("Joined room '");
                    const char* name_end = strchr(name_actual_start, '\'');
                    if (name_end) {
                        size_t len = name_end - name_actual_start;
                        if (len > 0 && len < sizeof(room_name)) {
                            strncpy(room_name, name_actual_start, len);
                            // room_name is now extracted
                            char *new_window_title = g_strdup_printf("Chat Client - Room: %s", room_name);
                            g_idle_add(update_window_title_idle, new_window_title);
                        }
                    }
                }

                if (id_marker && sscanf(id_marker + 5, "%u", &joined_room_id) == 1) {
                    current_room_id_for_user_list = joined_room_id;
                    request_current_room_user_list(); // Request user list for the new room

                    // Enable Leave Room button
                    WidgetSensitivityData *leave_b_data = g_new(WidgetSensitivityData, 1);
                    leave_b_data->widget = leave_room_button; leave_b_data->sensitive = TRUE;
                    g_idle_add(set_widget_sensitive_idle, leave_b_data);
                }
            } 
            // Check if this is a "created room" success message to trigger user list request
            // AND update window title
            else if (pk_header.type == PACKET_TYPE_CREATE_ROOM &&
                     strstr(display_msg, "[Server] Room '") && // More specific check
                     strstr(display_msg, "created and joined.") &&
                     strstr(display_msg, "(ID: ")) {
                unsigned int created_room_id;
                const char *id_marker = strstr(display_msg, "(ID: ");
                const char *room_name_start_marker = strstr(display_msg, "[Server] Room '");
                char room_name[64] = {0};

                if (room_name_start_marker) {
                    const char* name_actual_start = room_name_start_marker + strlen("[Server] Room '");
                    const char* name_end = strchr(name_actual_start, '\'');
                    if (name_end) {
                        size_t len = name_end - name_actual_start;
                        if (len > 0 && len < sizeof(room_name)) {
                            strncpy(room_name, name_actual_start, len);
                            // room_name is now extracted
                            char *new_window_title = g_strdup_printf("Chat Client - Room: %s", room_name);
                            g_idle_add(update_window_title_idle, new_window_title);
                        }
                    }
                }
                
                if (id_marker && sscanf(id_marker + 5, "%u", &created_room_id) == 1) {
                    current_room_id_for_user_list = created_room_id;
                    request_current_room_user_list();

                    // Enable Leave Room button
                    WidgetSensitivityData *leave_b_data = g_new(WidgetSensitivityData, 1);
                    leave_b_data->widget = leave_room_button; leave_b_data->sensitive = TRUE;
                    g_idle_add(set_widget_sensitive_idle, leave_b_data);
                }
            }
            // If the client itself leaves a room, clear the user list AND reset window title.
            else if (pk_header.type == PACKET_TYPE_LEAVE_ROOM && 
                     strcmp(display_msg, "[Server] You left the room.") == 0) { // Exact match
                current_room_id_for_user_list = 0;
                request_current_room_user_list(); // This will clear the list in UI

                // Disable Leave Room button
                WidgetSensitivityData *leave_b_data = g_new(WidgetSensitivityData, 1);
                leave_b_data->widget = leave_room_button; leave_b_data->sensitive = FALSE;
                g_idle_add(set_widget_sensitive_idle, leave_b_data);

                g_idle_add(update_window_title_idle, g_strdup("Chat Client")); // Reset title
            }

            g_idle_add(append_message_to_view_idle, g_strdup(display_msg)); // Always append the message

            // If we are in a room and receive a server message (e.g., user join/leave/nick change broadcast)
            // refresh the user list for the current room.
            // Debug: Print information before checking refresh condition
            g_print("[CLIENT DEBUG] Msg type: %d, In Room ID: %u, Msg starts with [Server]: %d, Msg: \"%.*s\"\n",
                    pk_header.type,
                    current_room_id_for_user_list,
                    (pk_header.data_len > 0 && strncmp(display_msg, "[Server]", 8) == 0),
                    pk_header.data_len, display_msg);
            if (pk_header.type == PACKET_TYPE_MESSAGE && current_room_id_for_user_list > 0 &&
                strncmp(display_msg, "[Server]", strlen("[Server]")) == 0) { // Check prefix for server messages
                g_print("[CLIENT DEBUG] Refresh condition MET. Requesting user list for room %u.\n", current_room_id_for_user_list);
                request_current_room_user_list();
            }

            if(data_buffer) { free(data_buffer); data_buffer = NULL; }
        }
        else if (data_buffer) { // Catch-all to free data_buffer if it was allocated but not handled
            free(data_buffer); data_buffer = NULL;
        }
    }


    // Cleanup if loop broke while buffer allocated
    if (data_buffer) free(data_buffer);
    if (full_payload_buffer) free(full_payload_buffer);

    // This part is reached if recv fails or loop terminates
    if (sock != -1) { // If not already closed by on_window_destroy
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Disconnected from server."));
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
    WidgetSensitivityData *room_name_data = g_new(WidgetSensitivityData, 1);
    room_name_data->widget = room_name_entry; room_name_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, room_name_data);
    WidgetSensitivityData *create_room_b_data = g_new(WidgetSensitivityData, 1);
    create_room_b_data->widget = create_room_button; create_room_b_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, create_room_b_data);
    WidgetSensitivityData *list_rooms_b_data = g_new(WidgetSensitivityData, 1);
    list_rooms_b_data->widget = list_rooms_button; list_rooms_b_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, list_rooms_b_data);
    // Add leave_room_button sensitivity update on disconnect
    WidgetSensitivityData *leave_room_b_data_on_disconnect = g_new(WidgetSensitivityData, 1);
    leave_room_b_data_on_disconnect->widget = leave_room_button; leave_room_b_data_on_disconnect->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, leave_room_b_data_on_disconnect);
    WidgetSensitivityData *nick_entry_data = g_new(WidgetSensitivityData, 1);
    nick_entry_data->widget = nickname_entry; nick_entry_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, nick_entry_data);
    WidgetSensitivityData *set_nick_b_data = g_new(WidgetSensitivityData, 1);
    set_nick_b_data->widget = set_nickname_button; set_nick_b_data->sensitive = FALSE;
    g_idle_add(set_widget_sensitive_idle, set_nick_b_data);
    
    // Reset current nickname display label on disconnect
    g_idle_add(update_nickname_display_label_idle, g_strdup("Current Nick: (None)"));
    // Reset window title on disconnect
    g_idle_add(update_window_title_idle, g_strdup("Chat Client"));

    if (sock != -1) { // Ensure socket is closed if connection dropped
        close(sock);
        sock = -1;
    }
    recv_thread_id = 0; // Mark thread as finished
    return NULL;
}

ssize_t send_client_packet(int sockfd, uint8_t type, const void *data, uint16_t data_len) {
    if (sockfd < 0) return -1;

    size_t packet_payload_size = HEADER_SIZE + data_len;
    size_t total_packet_size = packet_payload_size + 1; // +1 for checksum
    unsigned char *packet_buffer = malloc(total_packet_size);
    if (!packet_buffer) {
        perror("malloc for send_client_packet buffer failed");
        return -1;
    }

    PacketHeader *header = (PacketHeader *)packet_buffer;
    header->magic = htons(REQ_MAGIC);
    header->type = type;
    header->data_len = htons(data_len);

    if (data && data_len > 0) {
        memcpy(packet_buffer + HEADER_SIZE, data, data_len);
    }
    packet_buffer[packet_payload_size] = calculate_checksum(packet_buffer, packet_payload_size);
    ssize_t sent_bytes = send(sockfd, packet_buffer, total_packet_size, 0);
    free(packet_buffer);
    return sent_bytes == total_packet_size ? sent_bytes : -1;
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
    gtk_widget_set_sensitive(room_name_entry, TRUE);
    gtk_widget_set_sensitive(create_room_button, TRUE);
    gtk_widget_set_sensitive(list_rooms_button, TRUE);
    gtk_widget_set_sensitive(leave_room_button, FALSE); // Initially FALSE, enabled when in a room
    gtk_widget_set_sensitive(nickname_entry, TRUE);
    gtk_widget_set_sensitive(set_nickname_button, TRUE);
    gtk_widget_grab_focus(message_entry);

    if (pthread_create(&recv_thread_id, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create for receive_messages");
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Failed to create receive thread."));
        close(sock); sock = -1;
        gtk_widget_set_sensitive(connect_button, TRUE);
        gtk_widget_set_sensitive(send_button, FALSE);
        gtk_widget_set_sensitive(message_entry, FALSE);
        gtk_widget_set_sensitive(room_name_entry, FALSE);
        gtk_widget_set_sensitive(create_room_button, FALSE);
        gtk_widget_set_sensitive(list_rooms_button, FALSE);
        gtk_widget_set_sensitive(leave_room_button, FALSE);
        gtk_widget_set_sensitive(nickname_entry, FALSE);
        gtk_widget_set_sensitive(set_nickname_button, FALSE);
    }
}

// Callback for the "Send" button or pressing Enter in the message entry
static void on_send_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    const char *message = gtk_entry_get_text(GTK_ENTRY(message_entry));
    size_t msg_len = strlen(message);
    if (msg_len == 0) return;

    // All text from message_entry is now sent as a regular chat message.
    // Other packet types (CREATE_ROOM, JOIN_ROOM, LIST_ROOMS, etc.) are sent via their respective UI buttons/dialogs.
    if (msg_len > BUFFER_SIZE) { // Prevent buffer overflow if message is extremely long
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Message too long."));
        return;
    }

    if (send_client_packet(sock, PACKET_TYPE_MESSAGE, message, msg_len) < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send packet."));
    }

    gtk_entry_set_text(GTK_ENTRY(message_entry), "");
    gtk_widget_grab_focus(message_entry);
}

// Callback for the "Create Room" button
static void on_create_room_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    const char *room_name = gtk_entry_get_text(GTK_ENTRY(room_name_entry));
    size_t room_name_len = strlen(room_name);

    if (room_name_len == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Room name cannot be empty."));
        return;
    }
    if (room_name_len >= 64) { // Same as server-side check in SerializableRoomInfo
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Room name is too long (max 63 chars)."));
        return;
    }

    if (send_client_packet(sock, PACKET_TYPE_CREATE_ROOM, room_name, room_name_len) < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send create room request."));
    }
    gtk_entry_set_text(GTK_ENTRY(room_name_entry), "");
}

// Callback for the "Set Nickname" button
static void on_set_nickname_button_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    const char *new_nickname = gtk_entry_get_text(GTK_ENTRY(nickname_entry));
    size_t nick_len = strlen(new_nickname);

    if (nick_len == 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Nickname cannot be empty."));
        return;
    }
    if (nick_len >= 64) { // Server's Client.nick is char nick[64]
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Nickname is too long (max 63 chars)."));
        return;
    }

    if (send_client_packet(sock, PACKET_TYPE_SET_NICKNAME, new_nickname, nick_len) < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send set nickname request."));
    }
}

// Callback for the "List Rooms" button
static void on_list_rooms_button_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    // Request the server to send the list of rooms.
    // The response will be handled by receive_messages, which will then call show_or_update_room_list_idle.
    if (send_client_packet(sock, PACKET_TYPE_LIST_ROOMS, NULL, 0) < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send list rooms request."));
    }
}

// Callback for the "Leave Room" button
static void on_leave_room_clicked(GtkWidget *widget, gpointer data) {
    if (sock < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected to server."));
        return;
    }
    // No data is needed for PACKET_TYPE_LEAVE_ROOM
    if (send_client_packet(sock, PACKET_TYPE_LEAVE_ROOM, NULL, 0) < 0) {
        g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send leave room request."));
    }
    // Server's response will trigger UI updates (like disabling this button if successful,
    // which is handled in receive_messages)
}


// Response handler for the room list dialog
static void on_room_list_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    if (response_id == GTK_RESPONSE_ACCEPT) { // "Join Room" button
        GtkWidget* tree_view = GTK_WIDGET(g_object_get_data(G_OBJECT(dialog), "tree_view"));
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
        GtkTreeModel *model;
        GtkTreeIter iter;

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            guint room_id_to_join;
            gtk_tree_model_get(model, &iter, ROOM_LIST_COL_ID, &room_id_to_join, -1);
            if (sock >= 0) {
                // Client sends room_id in host byte order for join request
                if (send_client_packet(sock, PACKET_TYPE_JOIN_ROOM, &room_id_to_join, sizeof(guint)) < 0) {
                    g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send join room request."));
                }
            } else {
                g_idle_add(append_message_to_view_idle, g_strdup("[Client] Not connected. Cannot join room."));
            }
            gtk_widget_destroy(GTK_WIDGET(dialog)); // Close dialog after attempting to join
        } else {
             g_idle_add(append_message_to_view_idle, g_strdup("[Client] No room selected to join."));
             // Do not close dialog if nothing was selected, allow user to select or refresh/close
        }
    } else if (response_id == GTK_RESPONSE_APPLY) { // "Refresh" button
        // Send another request to list rooms
        if (sock >= 0) {
            if (send_client_packet(sock, PACKET_TYPE_LIST_ROOMS, NULL, 0) < 0) {
                 g_idle_add(append_message_to_view_idle, g_strdup("[Client] Error: Failed to send refresh rooms request."));
            }
        }
        // Do not destroy dialog on refresh, it will be updated by the response
    } else { // GTK_RESPONSE_CLOSE, GTK_RESPONSE_DELETE_EVENT, or any other response
        gtk_widget_destroy(GTK_WIDGET(dialog)); // This will trigger the "destroy" signal we connected
    }
}

// Window destroy callback (cleanup)
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (sock != -1) {
        shutdown(sock, SHUT_RDWR); // Gracefully shutdown socket to unblock recv thread
        close(sock);
        sock = -1;
    }
    if (recv_thread_id != 0) { // If thread was created and potentially running
        pthread_join(recv_thread_id, NULL); // Wait for the receive thread to finish
    }
    gtk_main_quit(); // Quit GTK main loop
}

// Application activate callback (builds UI)
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *main_grid, *controls_grid, *chat_user_grid, *ip_label, *port_label, *room_name_label, *controls_box, *nickname_label, *user_list_scrolled_window;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL); // Connect cleanup on window close

    main_grid = gtk_grid_new(); // Main grid for overall layout
    gtk_grid_set_row_spacing(GTK_GRID(main_grid), 10);
    gtk_container_add(GTK_CONTAINER(window), main_grid);

    // Grid for connection and room/nick controls (top part)
    controls_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(controls_grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(controls_grid), 5);
    gtk_grid_attach(GTK_GRID(main_grid), controls_grid, 0, 0, 1, 1);

    ip_label = gtk_label_new("Server IP:");
    gtk_grid_attach(GTK_GRID(controls_grid), ip_label, 0, 0, 1, 1);
    ip_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ip_entry), "127.0.0.1");
    gtk_grid_attach(GTK_GRID(controls_grid), ip_entry, 1, 0, 1, 1);

    port_label = gtk_label_new("Port:");
    gtk_grid_attach(GTK_GRID(controls_grid), port_label, 2, 0, 1, 1);
    port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(port_entry), "9000");
    gtk_grid_attach(GTK_GRID(controls_grid), port_entry, 3, 0, 1, 1);

    connect_button = gtk_button_new_with_label("Connect");
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_grid_attach(GTK_GRID(controls_grid), connect_button, 4, 0, 1, 1);

    controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); // 5px spacing
    gtk_grid_attach(GTK_GRID(controls_grid), controls_box, 0, 1, 5, 1); // Attach to controls_grid

    room_name_label = gtk_label_new("Room Name:");
    gtk_box_pack_start(GTK_BOX(controls_box), room_name_label, FALSE, FALSE, 0);
    room_name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(room_name_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(controls_box), room_name_entry, TRUE, TRUE, 0);
    gtk_widget_set_sensitive(room_name_entry, FALSE);
    create_room_button = gtk_button_new_with_label("Create Room");
    g_signal_connect(create_room_button, "clicked", G_CALLBACK(on_create_room_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(controls_box), create_room_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(create_room_button, FALSE);

    list_rooms_button = gtk_button_new_with_label("List Rooms");
    g_signal_connect(list_rooms_button, "clicked", G_CALLBACK(on_list_rooms_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(controls_box), list_rooms_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(list_rooms_button, FALSE);

    leave_room_button = gtk_button_new_with_label("Leave Room");
    g_signal_connect(leave_room_button, "clicked", G_CALLBACK(on_leave_room_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(controls_box), leave_room_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(leave_room_button, FALSE); // Initially disabled

    nickname_label = gtk_label_new("New Nickname:");
    gtk_box_pack_start(GTK_BOX(controls_box), nickname_label, FALSE, FALSE, 0);
    nickname_entry = gtk_entry_new();
    gtk_widget_set_hexpand(nickname_entry, TRUE); // Allow it to expand
    gtk_box_pack_start(GTK_BOX(controls_box), nickname_entry, TRUE, TRUE, 0);
    gtk_widget_set_sensitive(nickname_entry, FALSE);
    set_nickname_button = gtk_button_new_with_label("Set Nickname");
    g_signal_connect(set_nickname_button, "clicked", G_CALLBACK(on_set_nickname_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(controls_box), set_nickname_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(set_nickname_button, FALSE);

    // Grid for Chat View and User List (middle part)
    chat_user_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(chat_user_grid), 10);
    gtk_widget_set_vexpand(chat_user_grid, TRUE); // Make this grid expand vertically
    gtk_grid_attach(GTK_GRID(main_grid), chat_user_grid, 0, 1, 1, 1);

    // Chat View (Left side of chat_user_grid)
    scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC); // Add scrollbars
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_hexpand(scrolled_window, TRUE); // Chat view should expand horizontally
    chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled_window), chat_view);
    gtk_grid_attach(GTK_GRID(chat_user_grid), scrolled_window, 0, 0, 1, 1); // Col 0 of chat_user_grid

    // User List Panel (Right side of chat_user_grid)
    user_list_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(user_list_scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(user_list_scrolled_window, TRUE);
    gtk_widget_set_size_request(user_list_scrolled_window, 150, -1); // Set a default width for user list

    user_list_store = gtk_list_store_new(USER_LIST_NUM_COLS_MAIN_PANEL, G_TYPE_STRING);
    user_list_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    g_object_unref(user_list_store); // TreeView owns it now
    GtkCellRenderer *user_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *user_column = gtk_tree_view_column_new_with_attributes("In Room", user_renderer, "text", USER_LIST_COL_NICKNAME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_list_tree_view), user_column);
    gtk_container_add(GTK_CONTAINER(user_list_scrolled_window), user_list_tree_view);
    gtk_grid_attach(GTK_GRID(chat_user_grid), user_list_scrolled_window, 1, 0, 1, 1); // Col 1 of chat_user_grid

    // Message Entry and Send Button (bottom part)
    message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(message_entry, TRUE);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_clicked), NULL); // Connect activate for Enter key
    gtk_widget_set_sensitive(message_entry, FALSE);

    send_button = gtk_button_new_with_label("Send");
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_widget_set_sensitive(send_button, FALSE);

    // Re-adjusting main_grid for message_entry and send_button:
    GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    // Current Nickname Display Label - New Position (left of message_entry)
    current_nickname_display_label = gtk_label_new("Current Nick: (None)");
    gtk_box_pack_start(GTK_BOX(bottom_box), current_nickname_display_label, FALSE, FALSE, 5); // Add some padding (e.g., 5px to its right)

    gtk_box_pack_start(GTK_BOX(bottom_box), message_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bottom_box), send_button, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(main_grid), bottom_box, 0, 2, 1, 1);
    gtk_widget_set_sensitive(send_button, FALSE);

    gtk_widget_show_all(window);
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("org.example.chatclient", G_APPLICATION_NON_UNIQUE); // Allow multiple instances
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}