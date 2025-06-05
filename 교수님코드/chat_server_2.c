#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> // This was incomplete in the diff: fcntl. -> fcntl.h
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h> // For errno
#include <stdarg.h> // For va_list, va_start, va_end
#include <sys/time.h> // For struct timeval
#include <time.h> // For time()

// Protocol definitions
#define REQ_MAGIC 0x5a5a
#define RES_MAGIC 0xa5a5

typedef enum {
    PACKET_TYPE_MESSAGE = 0,        // C->S: chat message, S->C: broadcast chat message or server text message
    PACKET_TYPE_SET_NICKNAME = 1,   // C->S: request new nick, S->C: result / initial assignment
    PACKET_TYPE_CREATE_ROOM = 2,    // C->S: request, S->C: result
    PACKET_TYPE_JOIN_ROOM = 3,      // C->S: request, S->C: result
    PACKET_TYPE_LEAVE_ROOM = 4,     // C->S: request, S->C: result
    PACKET_TYPE_LIST_ROOMS = 5,     // C->S: request, S->C: room list data
    PACKET_TYPE_LIST_USERS = 6,     // C->S: request (data can be empty or room_id), S->C: user list data
} PacketType;

#pragma pack(push, 1) // Ensure no padding
typedef struct {
    uint16_t magic;
    uint8_t type;
    uint16_t data_len;
} PacketHeader;

typedef struct {
    uint32_t id;
    char name[64];
    uint16_t member_count;
} SerializableRoomInfo;
#pragma pack(pop)

#define HEADER_SIZE sizeof(PacketHeader)
#define PORTNUM 9000
#define BUFFER_SIZE 1024
#define MAX_EVENTS 10
// Forward declarations
typedef struct Client Client;
typedef struct Room Room;
void *client_process(void *arg);
void broadcast_room(Room *room, Client *sender, const char *format, ...);
void broadcast_lobby(Client *sender, const char *format, ...);
void list_add_client_unlocked(Client *cli);
void list_remove_client_unlocked(Client *cli);
Client *find_client_by_sock_unlocked(int sock);
Client *find_client_by_nick_unlocked(const char *nick);
void list_add_room_unlocked(Room *room);
void list_remove_room_unlocked(Room *room);
Room *find_room_unlocked(const char *name);
Room *find_room_by_id_unlocked(unsigned int id);
Room *find_room_by_id(unsigned int id);
void room_add_member_unlocked(Room *room, Client *cli);
void room_remove_member_unlocked(Room *room, Client *cli);
void destroy_room_if_empty_unlocked(Room *room);
void process_server_command(int epfd, int server_sock);
ssize_t send_packet(int sock, uint16_t magic, uint8_t type, const void *data, uint16_t data_len);
ssize_t recv_all(int sock, void *buf, size_t len);
unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length);
void broadcast_server_message_to_room(Room *room, Client *sender, const char *message_text);


typedef struct Client {
    int sock;
    char nick[64];
    pthread_t thread;
    struct Room *room;
    struct Client *next;
    struct Client *prev;
    struct Client *room_next;
    struct Client *room_prev;
} Client;

typedef struct Room {
    char name[64];
    unsigned int id; // Unique ID for the room
    struct Client *members;
    int member_count;
    struct Room *next;
    struct Room *prev;
} Room;

Client *g_clients = NULL;
Room *g_rooms = NULL;
static unsigned int g_next_room_id = 1; // For assigning unique room IDs

// Mutexes for thread-safe list access
pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER; // Protects global room list AND room member lists

unsigned char calculate_checksum(const unsigned char *header_and_data, size_t length) {
    unsigned char cs = 0;
    for (size_t i = 0; i < length; i++) {
        cs ^= header_and_data[i];
    }
    return cs;
}

ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t received = recv(sock, (char*)buf + total_received, len - total_received, 0);
        if (received <= 0) { // Error or connection closed
            return received;
        }
        total_received += received;
    }
    return total_received;
}

void list_add_client_unlocked(Client *cli) {
    if (g_clients == NULL) {
        g_clients = cli;
        cli->next = NULL;
        cli->prev = NULL;
    } else {
        Client *current = g_clients;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = cli;
        cli->prev = current;
        cli->next = NULL;
    }
}

void list_remove_client_unlocked(Client *cli) {
    if (cli->prev != NULL) {
        cli->prev->next = cli->next;
    } else {
        g_clients = cli->next;
    }
    if (cli->next != NULL) {
        cli->next->prev = cli->prev;
    }
    cli->prev = NULL;
    cli->next = NULL;
}

Client *find_client_by_sock_unlocked(int sock) {
    Client *current = g_clients;
    while (current != NULL) {
        if (current->sock == sock) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

Client *find_client_by_nick_unlocked(const char *nick) {
    Client *current = g_clients;
    while (current != NULL) {
        if (strcmp(current->nick, nick) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void list_add_room_unlocked(Room *room) {
    if (g_rooms == NULL) {
        g_rooms = room;
        room->next = NULL;
        room->prev = NULL;
    } else {
        Room *current = g_rooms;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = room;
        room->prev = current;
        room->next = NULL;
    }
}

void list_remove_room_unlocked(Room *room) {
    if (room->prev != NULL) {
        room->prev->next = room->next;
    } else {
        g_rooms = room->next;
    }
    if (room->next != NULL) {
        room->next->prev = room->prev;
    }
    room->prev = NULL;
    room->next = NULL;
}

Room *find_room_unlocked(const char *name) {
    Room *current = g_rooms;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Find room by ID (caller must hold g_rooms_mutex)
Room *find_room_by_id_unlocked(unsigned int id) {
    Room *current = g_rooms;
    while (current != NULL) {
        if (current->id == id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void room_add_member_unlocked(Room *room, Client *cli) {
    if (room->members == NULL) {
        room->members = cli;
        cli->room_next = NULL;
        cli->room_prev = NULL;
    } else {
        Client *current = room->members;
        while (current->room_next != NULL) {
            current = current->room_next;
        }
        current->room_next = cli;
        cli->room_prev = current;
        cli->room_next = NULL;
    }
    cli->room = room;
    room->member_count++;
}

void room_remove_member_unlocked(Room *room, Client *cli) {
    if (cli->room_prev != NULL) {
        cli->room_prev->room_next = cli->room_next;
    } else {
        room->members = cli->room_next;
    }
    if (cli->room_next != NULL) {
        cli->room_next->room_prev = cli->room_prev;
    }
    cli->room_prev = NULL;
    cli->room_next = NULL;
    cli->room = NULL; // Important: Clear client's room pointer
    room->member_count--;
}

void destroy_room_if_empty_unlocked(Room *room) {
    if (room->member_count <= 0) {
        printf("[INFO] Room '%s' is empty, destroying.\n", room->name);
        list_remove_room_unlocked(room);
        free(room);
    }
}

void list_add_client(Client *cli) {
    pthread_mutex_lock(&g_clients_mutex);
    list_add_client_unlocked(cli);
    pthread_mutex_unlock(&g_clients_mutex);
}

void list_remove_client(Client *cli) {
    pthread_mutex_lock(&g_clients_mutex);
    list_remove_client_unlocked(cli);
    pthread_mutex_unlock(&g_clients_mutex);
}

Client *find_client_by_sock(int sock) {
    pthread_mutex_lock(&g_clients_mutex);
    Client *cli = find_client_by_sock_unlocked(sock);
    pthread_mutex_unlock(&g_clients_mutex);
    return cli;
}

Client *find_client_by_nick(const char *nick) {
    pthread_mutex_lock(&g_clients_mutex);
    Client *cli = find_client_by_nick_unlocked(nick);
    pthread_mutex_unlock(&g_clients_mutex);
    return cli;
}

void list_add_room(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    list_add_room_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

Room *find_room(const char *name) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_unlocked(name);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}

Room *find_room_by_id(unsigned int id) {
    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = find_room_by_id_unlocked(id);
    pthread_mutex_unlock(&g_rooms_mutex);
    return room;
}


void room_add_member(Room *room, Client *cli) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_add_member_unlocked(room, cli);
    pthread_mutex_unlock(&g_rooms_mutex);
}

void room_remove_member(Room *room, Client *cli) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_remove_member_unlocked(room, cli);
    pthread_mutex_unlock(&g_rooms_mutex);
}

void destroy_room_if_empty(Room *room) {
    pthread_mutex_lock(&g_rooms_mutex);
    destroy_room_if_empty_unlocked(room);
    pthread_mutex_unlock(&g_rooms_mutex);
}

ssize_t send_packet(int sock, uint16_t magic, uint8_t type, const void *data, uint16_t data_len) {
    if (sock < 0) return -1;

    size_t packet_payload_size = HEADER_SIZE + data_len;
    size_t total_packet_size = packet_payload_size + 1; // +1 for checksum
    unsigned char *packet_buffer = malloc(total_packet_size);
    if (!packet_buffer) {
        perror("malloc for send_packet buffer failed");
        return -1;
    }

    PacketHeader *header = (PacketHeader *)packet_buffer;
    header->magic = htons(magic);
    header->type = type;
    header->data_len = htons(data_len);

    if (data && data_len > 0) {
        memcpy(packet_buffer + HEADER_SIZE, data, data_len);
    }

    packet_buffer[packet_payload_size] = calculate_checksum(packet_buffer, packet_payload_size);

    ssize_t sent_bytes = send(sock, packet_buffer, total_packet_size, 0); // send_all could be used here too
    free(packet_buffer);

    if (sent_bytes != total_packet_size) return -1;
    return sent_bytes;
}
void broadcast_room(Room *room, Client *sender, const char *format, ...) {
    if (!room) return; // Cannot broadcast to a null room

    char message[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    pthread_mutex_lock(&g_rooms_mutex);
    Client *member = room->members;
    while (member != NULL) {
        // A more robust check might involve a flag in the Client struct for validity
        if (member != sender && member->sock >= 0) {
            send_packet(member->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, message, strlen(message));
        }
        member = member->room_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

void broadcast_lobby(Client *sender, const char *format, ...) {
    char message[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    pthread_mutex_lock(&g_clients_mutex);
    Client *client = g_clients;
    while (client != NULL) {
        if (client != sender && client->room == NULL && client->sock >= 0) {
             send_packet(client->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, message, strlen(message));
        }
        client = client->next;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

void broadcast_server_message_to_room(Room *room, Client *sender_cli_to_exclude, const char *message_text) {
    if (!room || !message_text) return;

    pthread_mutex_lock(&g_rooms_mutex);
    Client *member = room->members;
    while(member != NULL) {
        if (member != sender_cli_to_exclude && member->sock >= 0) {
            send_packet(member->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, message_text, strlen(message_text));
        }
        member = member->room_next;
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

void process_create_room_request(Client *cli, const char *room_name_data, uint16_t room_name_len) {
    char room_name[sizeof(((Room*)0)->name)];
    char response_msg[BUFFER_SIZE];

    if (room_name_len == 0 || room_name_len >= sizeof(room_name)) {
        snprintf(response_msg, sizeof(response_msg), "[Server] Invalid room name length.");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_CREATE_ROOM, response_msg, strlen(response_msg));
        return;
    }
    memcpy(room_name, room_name_data, room_name_len);
    room_name[room_name_len] = '\0';

    if (cli->room != NULL) {
        snprintf(response_msg, sizeof(response_msg), "[Server] You are already in a room. Please /leave first.");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_CREATE_ROOM, response_msg, strlen(response_msg));
        return;
    }

    pthread_mutex_lock(&g_rooms_mutex);
    Room *existing_room_check = find_room_unlocked(room_name); // Check name before assigning ID
    if (existing_room_check != NULL) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_CREATE_ROOM, error_msg, strlen(error_msg));
        return;
    }
    // ID assignment and adding to list are done under this lock
    unsigned int new_id = g_next_room_id++;

    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new room failed");
        snprintf(response_msg, sizeof(response_msg), "[Server] Failed to create room (server error).");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_CREATE_ROOM, response_msg, strlen(response_msg));
        return;
    }
    strncpy(new_room->name, room_name, sizeof(new_room->name) - 1);
    new_room->name[sizeof(new_room->name) - 1] = '\0';
    new_room->id = new_id; // Assign the new ID
    new_room->members = NULL;
    new_room->member_count = 0;
    new_room->next = NULL;
    new_room->prev = NULL;

    list_add_room_unlocked(new_room); // Add to list while still holding lock
    pthread_mutex_unlock(&g_rooms_mutex);

    // Add member (this will re-lock g_rooms_mutex internally, which is fine)
    room_add_member(new_room, cli);

    printf("[INFO] Client %s created room '%s' (ID: %u) and joined.\n", cli->nick, new_room->name, new_room->id);
    snprintf(response_msg, sizeof(response_msg), "[Server] Room '%s' (ID: %u) created and joined.", new_room->name, new_room->id);
    send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_CREATE_ROOM, response_msg, strlen(response_msg));
}

void process_join_room_request(Client *cli, const void *data, uint16_t data_len) {
    char response_msg[BUFFER_SIZE];
    if (data_len != sizeof(unsigned int)) {
        snprintf(response_msg, sizeof(response_msg), "[Server] Invalid data for join room request.");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_JOIN_ROOM, response_msg, strlen(response_msg));
        return;
    }
    if (cli->room != NULL) {
        snprintf(response_msg, sizeof(response_msg), "[Server] You are already in a room. Please /leave first.");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_JOIN_ROOM, response_msg, strlen(response_msg));
        return;
    }

    unsigned int room_id_to_join;
    memcpy(&room_id_to_join, data, sizeof(unsigned int));
    // No need for ntohl if client sends it as host byte order uint and we memcpy.
    // If client sends network byte order, then room_id_to_join = ntohl(*(unsigned int*)data);

    Room *target_room = find_room_by_id(room_id_to_join);
    if (!target_room) {
        snprintf(response_msg, sizeof(response_msg), "[Server] Room with ID %u not found.", room_id_to_join);
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_JOIN_ROOM, response_msg, strlen(response_msg));
        return;
    }
    room_add_member(target_room, cli);
    printf("[INFO] Client %s joined room '%s' (ID: %u).\n", cli->nick, target_room->name, target_room->id);
    
    snprintf(response_msg, sizeof(response_msg), "[Server] Joined room '%s' (ID: %u).", target_room->name, target_room->id);
    send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_JOIN_ROOM, response_msg, strlen(response_msg));

    char broadcast_join_msg[BUFFER_SIZE];
    snprintf(broadcast_join_msg, sizeof(broadcast_join_msg), "[Server] %s joined the room.", cli->nick);
    broadcast_server_message_to_room(target_room, cli, broadcast_join_msg);
}

void process_leave_room_request(Client *cli) {
    char response_msg[BUFFER_SIZE];
    if (cli->room == NULL) {
        snprintf(response_msg, sizeof(response_msg), "[Server] You are not in any room.");
        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_LEAVE_ROOM, response_msg, strlen(response_msg));
        return;
    }
    Room *current_room = cli->room;
    char broadcast_leave_msg[BUFFER_SIZE];
    snprintf(broadcast_leave_msg, sizeof(broadcast_leave_msg), "[Server] %s left the room.", cli->nick);
    
    room_remove_member(current_room, cli); // This also sets cli->room = NULL
    printf("[INFO] Client %s left room '%s'.\n", cli->nick, current_room->name);

    snprintf(response_msg, sizeof(response_msg), "[Server] You left the room.");
    send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_LEAVE_ROOM, response_msg, strlen(response_msg));

    // Broadcast after sending confirmation to the leaving client
    broadcast_server_message_to_room(current_room, NULL, broadcast_leave_msg); // Send to all, including former member if they were not fully disconnected

    destroy_room_if_empty(current_room);
}

void get_nickname(Client *user) {
    char temp_nick_buffer[sizeof(user->nick)];
    int attempt = 0;
    const int MAX_NICK_GENERATION_ATTEMPTS = 10;

    pthread_mutex_lock(&g_clients_mutex);
    while (attempt < MAX_NICK_GENERATION_ATTEMPTS) {
        snprintf(temp_nick_buffer, sizeof(temp_nick_buffer), "User%d", rand() % 100000);
        // The current 'user' is in g_clients but its 'nick' field is not yet 'temp_nick_buffer'.
        // find_client_by_nick_unlocked checks against other clients' established nicks.
        Client *existing_client = find_client_by_nick_unlocked(temp_nick_buffer);
        
        if (existing_client == NULL) { // Unique name found
            strncpy(user->nick, temp_nick_buffer, sizeof(user->nick) - 1);
            user->nick[sizeof(user->nick) - 1] = '\0';
            break; 
        }
        attempt++;
    }
    pthread_mutex_unlock(&g_clients_mutex);

    if (attempt == MAX_NICK_GENERATION_ATTEMPTS) {
        // Fallback if unique random name generation failed
        long fallback_suffix = (long)time(NULL) ^ (long)(intptr_t)user;
        snprintf(user->nick, sizeof(user->nick), "Guest%ld", fallback_suffix % 100000);
        user->nick[sizeof(user->nick) - 1] = '\0';
    }

    printf("[INFO] New user connected: %s (fd %d)\n", user->nick, user->sock);
    
    // Send assigned nickname to client
    send_packet(user->sock, RES_MAGIC, PACKET_TYPE_SET_NICKNAME, user->nick, strlen(user->nick));

    // Send welcome message
    char welcome_text[BUFFER_SIZE];
    snprintf(welcome_text, sizeof(welcome_text),
             "[Server] Welcome! Your assigned nickname is %s.\n"
             "[Server] Use UI buttons for other actions (create/join/list rooms).\n", user->nick);
    send_packet(user->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, welcome_text, strlen(welcome_text));
}

void *client_process(void *arg) {
    Client *cli = (Client *)arg;
    PacketHeader pk_header;
    unsigned char *data_buffer = NULL;
    unsigned char *full_payload_buffer = NULL; // For checksum calculation [header + data]

    get_nickname(cli);

    while (cli->sock >= 0) {
        // 1. Read Header
        ssize_t header_bytes = recv_all(cli->sock, &pk_header, HEADER_SIZE);
        if (header_bytes <= 0) {
            if (header_bytes == 0) printf("[INFO] Client %s (fd %d) disconnected (header read).\n", cli->nick, cli->sock);
            else if (errno != EAGAIN && errno != EWOULDBLOCK) perror("recv_all for header failed");
            break;
        }
        
        // 서버->클라이언트로 보낼 때랑 클라이언트->서버로 보낼 때 바이트오더(리틀엔디언,빅엔디언)를 염두에 두어야 함. ntohs, htons 이거
        pk_header.magic = ntohs(pk_header.magic);
        pk_header.data_len = ntohs(pk_header.data_len);

        if (pk_header.magic != REQ_MAGIC) {
            printf("[WARN] Client %s (fd %d) sent bad magic: 0x%x. Disconnecting.\n", cli->nick, cli->sock, pk_header.magic);
            break; 
        }

        if (pk_header.data_len > BUFFER_SIZE * 2) { // Sanity check
            printf("[WARN] Client %s (fd %d) sent too large data_len: %u. Disconnecting.\n", cli->nick, cli->sock, pk_header.data_len);
            break;
        }

        // Prepare buffer for header + data (for checksum calculation)
        size_t payload_size = HEADER_SIZE + pk_header.data_len;
        full_payload_buffer = malloc(payload_size);
        if (!full_payload_buffer) {
            perror("malloc for full_payload_buffer failed");
            break;
        }

        // Re-construct header in network byte order for checksum calculation consistency
        PacketHeader temp_header_for_checksum;
        temp_header_for_checksum.magic = htons(REQ_MAGIC);
        temp_header_for_checksum.type = pk_header.type; // single byte
        temp_header_for_checksum.data_len = htons(pk_header.data_len);
        memcpy(full_payload_buffer, &temp_header_for_checksum, HEADER_SIZE);

        // 2. Read Data (if any)
        if (pk_header.data_len > 0) {
            data_buffer = malloc(pk_header.data_len); // Separate buffer for actual data processing
            if (!data_buffer) {
                perror("malloc for data_buffer failed");
                free(full_payload_buffer); full_payload_buffer = NULL;
                break;
            }
            ssize_t data_bytes = recv_all(cli->sock, data_buffer, pk_header.data_len);
            if (data_bytes <= 0) {
                if (data_bytes == 0) printf("[INFO] Client %s (fd %d) disconnected (data read).\n", cli->nick, cli->sock);
                else perror("recv_all for data failed");
                free(data_buffer); data_buffer = NULL;
                free(full_payload_buffer); full_payload_buffer = NULL;
                break;
            }
            memcpy(full_payload_buffer + HEADER_SIZE, data_buffer, pk_header.data_len);
        } else {
            data_buffer = NULL;
        }

        // 3. Read Checksum
        unsigned char received_checksum;
        ssize_t checksum_byte = recv_all(cli->sock, &received_checksum, 1);
        if (checksum_byte <= 0) {
            if (checksum_byte == 0) printf("[INFO] Client %s (fd %d) disconnected (checksum read).\n", cli->nick, cli->sock);
            else perror("recv_all for checksum failed");
            if (data_buffer) free(data_buffer); data_buffer = NULL;
            free(full_payload_buffer); full_payload_buffer = NULL;
            break;
        }

        // 4. Verify Checksum
        unsigned char calculated_cs = calculate_checksum(full_payload_buffer, payload_size);
        free(full_payload_buffer); full_payload_buffer = NULL;

        if (calculated_cs != received_checksum) {
            printf("[WARN] Client %s (fd %d) checksum mismatch. Expected %02x, Got %02x. Ignoring packet.\n", cli->nick, cli->sock, calculated_cs, received_checksum);
            if (data_buffer) { free(data_buffer); data_buffer = NULL; }
            continue; 
        }

        // 5. Process Packet
        char response_text[BUFFER_SIZE]; // For simple text responses

        switch (pk_header.type) {
            case PACKET_TYPE_MESSAGE: {
                if (data_buffer && pk_header.data_len > 0) {
                    char msg_text[pk_header.data_len + 1];
                    memcpy(msg_text, data_buffer, pk_header.data_len);
                    msg_text[pk_header.data_len] = '\0';

                    if (cli->room != NULL) {
                        char full_room_msg[BUFFER_SIZE];
                        snprintf(full_room_msg, sizeof(full_room_msg), "[%s] %s", cli->nick, msg_text);
                        // Send the message back to the sender as well
                        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, full_room_msg, strlen(full_room_msg));
                        broadcast_server_message_to_room(cli->room, cli, full_room_msg);
                    } else {
                        snprintf(response_text, sizeof(response_text), "[Server] You must join a room to send messages.");
                        send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_MESSAGE, response_text, strlen(response_text));
                    }
                }
                break;
            }
            case PACKET_TYPE_SET_NICKNAME: {
                if (data_buffer && pk_header.data_len > 0 && pk_header.data_len < sizeof(cli->nick)) {
                    char new_nick[sizeof(cli->nick)];
                    memcpy(new_nick, data_buffer, pk_header.data_len);
                    new_nick[pk_header.data_len] = '\0';

                    Client *existing = find_client_by_nick(new_nick);
                    if (existing != NULL && existing != cli) {
                        snprintf(response_text, sizeof(response_text), "[Server] Nickname '%s' is already taken.", new_nick);
                    } else {
                        printf("[INFO] %s changed nickname to %s\n", cli->nick, new_nick);
                        char nick_change_broadcast[BUFFER_SIZE];
                        snprintf(nick_change_broadcast, sizeof(nick_change_broadcast), "[Server] %s is now known as %s.", cli->nick, new_nick);
                        
                        strncpy(cli->nick, new_nick, sizeof(cli->nick) - 1);
                        cli->nick[sizeof(cli->nick) - 1] = '\0';
                        snprintf(response_text, sizeof(response_text), "[Server] Nickname updated to %s.", cli->nick);

                        // Broadcast to lobby
                        broadcast_lobby(NULL, "%s", nick_change_broadcast); // NULL sender means server message
                        // Broadcast to current room if any
                        if (cli->room) {
                            broadcast_server_message_to_room(cli->room, NULL, nick_change_broadcast);
                        }
                    }
                } else {
                    snprintf(response_text, sizeof(response_text), "[Server] Invalid nickname format.");
                }
                send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_SET_NICKNAME, response_text, strlen(response_text));
                break;
            }
            case PACKET_TYPE_CREATE_ROOM:
                process_create_room_request(cli, (const char*)data_buffer, pk_header.data_len);
                break;
            case PACKET_TYPE_JOIN_ROOM:
                process_join_room_request(cli, data_buffer, pk_header.data_len);
                break;
            case PACKET_TYPE_LEAVE_ROOM:
                process_leave_room_request(cli);
                break;
            case PACKET_TYPE_LIST_ROOMS: {
                pthread_mutex_lock(&g_rooms_mutex);
                int room_count = 0;
                Room *r = g_rooms;
                while(r) { room_count++; r = r->next; }

                size_t list_data_size = sizeof(uint16_t) + room_count * sizeof(SerializableRoomInfo);
                unsigned char *room_list_data = malloc(list_data_size);
                if (!room_list_data) {
                    pthread_mutex_unlock(&g_rooms_mutex);
                    perror("malloc for room_list_data failed");
                    break;
                }
                uint16_t net_room_count = htons(room_count);
                memcpy(room_list_data, &net_room_count, sizeof(uint16_t));
                
                SerializableRoomInfo *sri_ptr = (SerializableRoomInfo*)(room_list_data + sizeof(uint16_t));
                r = g_rooms;
                for (int k=0; k<room_count; k++) {
                    sri_ptr[k].id = htonl(r->id); // Network byte order for multi-byte fields
                    strncpy(sri_ptr[k].name, r->name, sizeof(sri_ptr[k].name)-1);
                    sri_ptr[k].name[sizeof(sri_ptr[k].name)-1] = '\0';
                    sri_ptr[k].member_count = htons(r->member_count);
                    r = r->next;
                }
                pthread_mutex_unlock(&g_rooms_mutex);
                send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_LIST_ROOMS, room_list_data, list_data_size);
                free(room_list_data);
                break;
            }
            case PACKET_TYPE_LIST_USERS: {
                char user_list_str[BUFFER_SIZE * 2] = {0}; // Sufficient buffer for multiple users
                size_t current_len = 0;
                unsigned int target_room_id = 0;

                // Check if room ID is in request data
                if (pk_header.data_len == sizeof(unsigned int) && data_buffer != NULL) {
                    memcpy(&target_room_id, data_buffer, sizeof(unsigned int));
                    // Assume client sends room_id in host byte order
                }

                if (target_room_id > 0) { // Request for user list of a specific room
                    pthread_mutex_lock(&g_rooms_mutex); // Use g_rooms_mutex to access Room information
                    Room *room = find_room_by_id_unlocked(target_room_id);
                    if (room) {
                        Client *member = room->members;
                        while(member != NULL && (current_len + strlen(member->nick) + 1) < sizeof(user_list_str)) {
                            strcpy(user_list_str + current_len, member->nick);
                            current_len += strlen(member->nick);
                            user_list_str[current_len++] = '\0'; // Separate nicknames with null char
                            member = member->room_next;
                        }
                    }
                    pthread_mutex_unlock(&g_rooms_mutex);
                } else { // 전체 사용자 목록 요청 (data_len == 0 인 경우)
                    pthread_mutex_lock(&g_clients_mutex);
                    Client *c = g_clients;
                    while(c != NULL && (current_len + strlen(c->nick) + 1) < sizeof(user_list_str)) {
                        strcpy(user_list_str + current_len, c->nick);
                        current_len += strlen(c->nick);
                        user_list_str[current_len++] = '\0'; // Separate nicknames with null char
                        c = c->next;
                    }
                    pthread_mutex_unlock(&g_clients_mutex);
                }
                
                // current_len is the actual data length including null terminators
                send_packet(cli->sock, RES_MAGIC, PACKET_TYPE_LIST_USERS, user_list_str, current_len);
                break;
            }
            default:
                snprintf(response_text, sizeof(response_text), "[Server] Unknown packet type: %d", pk_header.type);
                send_packet(cli->sock, RES_MAGIC, pk_header.type, response_text, strlen(response_text)); // Echo type back
                continue;
        }

        if (data_buffer) {
            free(data_buffer);
            data_buffer = NULL;
        }
    } // while(cli->sock >=0)

    // Cleanup if loop exited while buffers were allocated
    if (data_buffer) free(data_buffer);
    if (full_payload_buffer) free(full_payload_buffer);

    printf("[INFO] Cleaning up client %s (fd %d).\n", cli->nick, cli->sock);
    if (cli->room) {
        Room *r = cli->room;
        char disconnect_msg[BUFFER_SIZE];
        snprintf(disconnect_msg, sizeof(disconnect_msg), "[Server] %s disconnected.", cli->nick);
        
        room_remove_member(r, cli); // Sets cli->room = NULL
        broadcast_server_message_to_room(r, NULL, disconnect_msg); // Broadcast after removal
        destroy_room_if_empty(r);
    }
    list_remove_client(cli);
    if (cli->sock >= 0) {
        close(cli->sock);
        cli->sock = -1;
    }
    free(cli);
    printf("[INFO] Client structure freed.\n");
    pthread_exit(NULL);
}

void process_server_command(int epfd, int server_sock) {
    char command_buf[64];
    if (fgets(command_buf, sizeof(command_buf) - 1, stdin) == NULL) {
        printf("\n[INFO] Server shutting down (EOF on stdin).\n");
        // Graceful shutdown sequence
        close(server_sock); // Stop accepting new connections
        pthread_mutex_lock(&g_clients_mutex);
        Client *current_cli = g_clients;
        while(current_cli != NULL) {
            if (current_cli->sock >= 0) {
                printf("[INFO] Closing socket for client %s (fd %d) for shutdown.\n", current_cli->nick, current_cli->sock);
                shutdown(current_cli->sock, SHUT_RDWR);
                close(current_cli->sock);
                current_cli->sock = -1;
            }
            current_cli = current_cli->next;
        }
        pthread_mutex_unlock(&g_clients_mutex);

        pthread_mutex_lock(&g_rooms_mutex);
        Room *current_room = g_rooms;
        while(current_room != NULL) {
            Room *next_room = current_room->next;
            printf("[INFO] Freeing room '%s' during shutdown.\n", current_room->name);
            free(current_room);
            current_room = next_room;
        }
        g_rooms = NULL;
        pthread_mutex_unlock(&g_rooms_mutex);
        close(epfd);
        exit(EXIT_SUCCESS);
    }
    command_buf[strcspn(command_buf, "\n")] = 0;
    char *cmd = strtok(command_buf, " ");

    if (!cmd || strlen(cmd) == 0) {
        ; // Explicit empty statement for clarity and to avoid potential parsing issues with empty blocks
    } 
    else if (strcmp(cmd, "users") == 0) {
        printf("--- Connected Users ---\n");
        pthread_mutex_lock(&g_clients_mutex);
        Client *current = g_clients;
        if (current == NULL) printf("No users connected.\n");
        else {
            while (current != NULL) {
                if (current->room) {
                    printf("  - %s (fd %d, Room: '%s' ID: %u)\n",
                           current->nick, current->sock, current->room->name, current->room->id);
                } else {
                    printf("  - %s (fd %d, Room: None)\n",
                           current->nick, current->sock);
                }
                current = current->next;
            }
        }
        pthread_mutex_unlock(&g_clients_mutex);
        printf("-----------------------\n");
    } else if (strcmp(cmd, "rooms") == 0) {
        printf("--- Chat Rooms ---\n");
        pthread_mutex_lock(&g_rooms_mutex);
        Room *current_room = g_rooms;
        if (current_room == NULL) printf("No rooms created.\n");
        else {
            while (current_room != NULL) {
                printf("  - ID %u: Room '%s' (%d members):\n", current_room->id, current_room->name, current_room->member_count);
                Client *member = current_room->members;
                while (member != NULL) {
                    printf("    - %s (fd %d)\n", member->nick, member->sock);
                    member = member->room_next;
                }
                current_room = current_room->next;
            }
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        printf("------------------\n");
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
         printf("[INFO] Server shutting down (command).\n");
         // Graceful shutdown (same logic as EOF)
         close(server_sock);
         pthread_mutex_lock(&g_clients_mutex);
         Client *current = g_clients;
         while(current != NULL) {
             if (current->sock >= 0) {
                 printf("[INFO] Closing socket for client %s (fd %d) for shutdown.\n", current->nick, current->sock);
                 shutdown(current->sock, SHUT_RDWR); close(current->sock); current->sock = -1;
             }
             current = current->next;
         }
         pthread_mutex_unlock(&g_clients_mutex);
         pthread_mutex_lock(&g_rooms_mutex);
         Room *current_room = g_rooms;
         while(current_room != NULL) {
             Room *next_room = current_room->next;
             printf("[INFO] Freeing room '%s' during shutdown.\n", current_room->name);
             free(current_room); current_room = next_room;
         }
         g_rooms = NULL; pthread_mutex_unlock(&g_rooms_mutex);
         close(epfd);
         exit(EXIT_SUCCESS);
    } else {
        printf("[Server] Unknown server command: %s. Available: users, rooms, quit\n", cmd);
    }
    printf("> "); fflush(stdout);
}

int main() {
    int server_sock;
    struct sockaddr_in sin, cli_addr;
    socklen_t clientlen = sizeof(cli_addr);

    // Seed random number generator
    srand(time(NULL));

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket creation failed"); exit(EXIT_FAILURE);
    }
    int optval = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt SO_REUSEADDR failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    memset((char *)&sin, '\0', sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORTNUM);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    if (listen(server_sock, 10) < 0) {
        perror("listen failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d\n", PORTNUM);

    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1 failed"); close(server_sock); exit(EXIT_FAILURE);
    }
    struct epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN; ev.data.fd = server_sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl: add server_sock failed"); close(epfd); close(server_sock); exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN; ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1) {
        perror("epoll_ctl: add STDIN_FILENO failed"); close(epfd); close(server_sock); exit(EXIT_FAILURE);
    }
    printf("> "); fflush(stdout);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed"); break;
        }
        for (int i = 0; i < nfds; i++) {
            int current_fd = events[i].data.fd;
            if (current_fd == server_sock) {
                int client_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &clientlen);
                if (client_sock < 0) {
                    // Handle accept errors, especially for non-blocking accept (though this is blocking)
                    perror("accept failed"); continue;
                }
                Client *new_client = (Client *)malloc(sizeof(Client));
                if (!new_client) {
                    perror("malloc for new client failed"); close(client_sock);
                    fprintf(stderr, "[ERROR] Failed to allocate memory for new client.\n"); continue;
                }
                memset(new_client, 0, sizeof(Client));
                new_client->sock = client_sock;

                // Add client to global list *before* thread creation for visibility.
                list_add_client(new_client);

                if (pthread_create(&(new_client->thread), NULL, client_process, new_client) != 0) {
                    perror("pthread_create failed");
                    list_remove_client(new_client); // Clean up if thread creation fails
                    close(new_client->sock); free(new_client);
                    fprintf(stderr, "[ERROR] Failed to create thread for new client.\n"); continue;
                }
                pthread_detach(new_client->thread); // Resources freed automatically on thread exit
            } else if (current_fd == STDIN_FILENO) {
                process_server_command(epfd, server_sock);
            }
            // Client sockets are handled by their respective threads.
            // Epoll in main only handles server_sock and stdin.
        }
    }
    // Cleanup if loop breaks unexpectedly (normally handled by 'quit' or EOF)
    close(epfd);
    close(server_sock);
    return 0;
}
