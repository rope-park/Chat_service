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

#define PORTNUM 9000
#define BUFFER_SIZE 1024
#define MAX_EVENTS 10

// Forward declarations
typedef struct Client Client;
typedef struct Room Room;
void *client_process(void *arg);
ssize_t safe_send(int sock, const char *msg);
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

ssize_t safe_send(int sock, const char *msg) {
    if (sock < 0 || msg == NULL) return -1;
    ssize_t len = strlen(msg);
    ssize_t total_sent = 0;
    ssize_t bytes_left = len;

    while (bytes_left > 0) {
        ssize_t sent = send(sock, msg + total_sent, bytes_left, 0);
        if (sent < 0) {
            // For blocking sockets, EAGAIN/EWOULDBLOCK is unlikely unless a timeout is set.
            // Any send error here is typically a more serious issue.
            perror("send error");
            return -1;
        }
        total_sent += sent;
        bytes_left -= sent;
    }
    return total_sent;
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
        if (/*member != sender &&*/ member->sock >= 0) {
            safe_send(member->sock, message);
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
             safe_send(client->sock, message);
        }
        client = client->next;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

void cmd_users(Client *cli) {
    char user_list[BUFFER_SIZE];
    user_list[0] = '\0';

    if (cli->room) {
        strcat(user_list, "[Server] Users in room '");
        strcat(user_list, cli->room->name);
        strcat(user_list, "': ");

        pthread_mutex_lock(&g_rooms_mutex);
        Client *member = cli->room->members;
        while (member != NULL) {
            strcat(user_list, member->nick);
            if (member->room_next != NULL) {
                strcat(user_list, ", ");
            }
            member = member->room_next;
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        strcat(user_list, "\n");
        safe_send(cli->sock, user_list);

    } else {
        strcat(user_list, "[Server] Connected users: ");
        pthread_mutex_lock(&g_clients_mutex);
        Client *client = g_clients;
        while (client != NULL) {
            strcat(user_list, client->nick);
            if (client->next != NULL) {
                strcat(user_list, ", ");
            }
            client = client->next;
        }
        pthread_mutex_unlock(&g_clients_mutex);
        strcat(user_list, "\n");
        safe_send(cli->sock, user_list);
    }
}

void cmd_rooms(int sock) {
    char room_list[BUFFER_SIZE];
    room_list[0] = '\0';
    strcat(room_list, "[Server] Available rooms: ");

    pthread_mutex_lock(&g_rooms_mutex);
    Room *room = g_rooms;
    if (room == NULL) {
        strcat(room_list, "No rooms available.\n");
    } else {
        while (room != NULL) {
            // Use snprintf carefully to avoid buffer overflow
            int remaining_len = BUFFER_SIZE - strlen(room_list) - 1;
            if (remaining_len <= 0) { strcat(room_list, "..."); break; }

            int written = snprintf(room_list + strlen(room_list), remaining_len,
                                   "ID %u: '%s' (%d members)", room->id, room->name, room->member_count);
            if (written < 0 || written >= remaining_len) {
                 strcat(room_list, "..."); break;
            }

            if (room->next != NULL) {
                remaining_len = BUFFER_SIZE - strlen(room_list) - 1;
                if (remaining_len > strlen(", ")) {
                    strcat(room_list, ", ");
                } else {
                    strcat(room_list, "..."); break;
                }
            }
            room = room->next;
        }
        strcat(room_list, "\n");
    }
    pthread_mutex_unlock(&g_rooms_mutex);
    safe_send(sock, room_list);
}

void cmd_dm(Client *sender, const char *target_nick, const char *message) {
    if (!target_nick || !message || strlen(target_nick) == 0 || strlen(message) == 0) {
        safe_send(sender->sock, "[Server] Usage: /dm <nickname> <message>\n");
        return;
    }
    Client *target_cli = find_client_by_nick(target_nick);
    if (!target_cli) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] User '%s' not found.\n", target_nick);
        safe_send(sender->sock, error_msg);
        return;
    }
    if (target_cli == sender) {
         safe_send(sender->sock, "[Server] You cannot DM yourself.\n");
         return;
    }
    char formatted_msg[BUFFER_SIZE];
    snprintf(formatted_msg, sizeof(formatted_msg), "[DM from %s] %s\n", sender->nick, message);
    safe_send(target_cli->sock, formatted_msg);
    safe_send(sender->sock, "[Server] DM sent.\n");
}

void cmd_create_room(Client *cli, const char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        safe_send(cli->sock, "[Server] Usage: /create <room_name>\n");
        return;
    }
    // Check max room name length using struct member size
    if (strlen(room_name) >= sizeof(((Room*)0)->name)) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name too long (max %zu characters).\n", sizeof(((Room*)0)->name) - 1);
        safe_send(cli->sock, error_msg);
        return;
    }
    if (cli->room != NULL) {
        safe_send(cli->sock, "[Server] You are already in a room. Please /leave first.\n");
        return;
    }

    // Check for existing room name and assign ID under lock
    pthread_mutex_lock(&g_rooms_mutex);
    Room *existing_room_check = find_room_unlocked(room_name); // Check name before assigning ID
    if (existing_room_check != NULL) {
        pthread_mutex_unlock(&g_rooms_mutex);
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room name '%s' already exists.\n", room_name);
        safe_send(cli->sock, error_msg);
        return;
    }
    // ID assignment and adding to list are done under this lock
    unsigned int new_id = g_next_room_id++;

    Room *new_room = (Room *)malloc(sizeof(Room));
    if (!new_room) {
        perror("malloc for new room failed");
        safe_send(cli->sock, "[Server] Failed to create room.\n");
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
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Room '%s' (ID: %u) created and joined.\n", new_room->name, new_room->id);
    safe_send(cli->sock, success_msg);
}

void cmd_join_room(Client *cli, const char *room_id_str) {
    if (!room_id_str || strlen(room_id_str) == 0) {
        safe_send(cli->sock, "[Server] Usage: /join <room_id>\n");
        return;
    }
    if (cli->room != NULL) {
        safe_send(cli->sock, "[Server] You are already in a room. Please /leave first.\n");
        return;
    }

    char *endptr;
    long num_id = strtol(room_id_str, &endptr, 10);

    // Validate room_id_str is a valid positive number
    if (*endptr != '\0' || num_id <= 0 || num_id >= g_next_room_id || num_id > 0xFFFFFFFF) { // UINT_MAX approx
        safe_send(cli->sock, "[Server] Invalid room ID. Please use a valid positive number.\n");
        return;
    }
    unsigned int room_id_to_join = (unsigned int)num_id;

    Room *target_room = find_room_by_id(room_id_to_join);
    if (!target_room) {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "[Server] Room with ID %u not found.\n", room_id_to_join);
        safe_send(cli->sock, error_msg);
        return;
    }
    room_add_member(target_room, cli);
    printf("[INFO] Client %s joined room '%s' (ID: %u).\n", cli->nick, target_room->name, target_room->id);
    char success_msg[BUFFER_SIZE];
    snprintf(success_msg, sizeof(success_msg), "[Server] Joined room '%s' (ID: %u).\n", target_room->name, target_room->id);
    safe_send(cli->sock, success_msg);
    broadcast_room(target_room, cli, "[Server] %s joined the room.\n", cli->nick);
}

void cmd_leave_room(Client *cli) {
    if (cli->room == NULL) {
        safe_send(cli->sock, "[Server] You are not in any room.\n");
        return;
    }
    Room *current_room = cli->room;
    broadcast_room(current_room, cli, "[Server] %s left the room.\n", cli->nick);
    room_remove_member(current_room, cli); // This also sets cli->room = NULL
    printf("[INFO] Client %s left room '%s'.\n", cli->nick, current_room->name);
    safe_send(cli->sock, "[Server] You left the room.\n");
    destroy_room_if_empty(current_room);
}

void get_nickname(Client *user) {
    // Assign a random nickname, e.g., "User12345"
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
    
    char welcome_msg[BUFFER_SIZE];
    snprintf(welcome_msg, sizeof(welcome_msg),
             "[Server] Welcome! Your assigned nickname is %s.\n"
             "[Server] To change it, type: /nick <new_nickname>\n"
             "[Server] Type /help for a list of commands.\n", user->nick);
    safe_send(user->sock, welcome_msg);
}

void *client_process(void *arg) {
    Client *cli = (Client *)arg;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    get_nickname(cli);
    // If get_nickname failed, cli->sock might be bad or cli->nick is "Guest".
    // The client struct is already added to the global list in main.

    while (cli->sock >= 0) {
        bytes_received = recv(cli->sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("[INFO] Client %s (fd %d) disconnected.\n", cli->nick, cli->sock);
            } else {
                // EAGAIN/EWOULDBLOCK might occur if RCVTIMEO was set and expired.
                // Otherwise, it's a more serious error.
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv error");
                    printf("[ERROR] Recv error on client %s (fd %d).\n", cli->nick, cli->sock);
                }
            }
            break;
        }
        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

        // printf("[DEBUG] Received from %s (fd %d): %s\n", cli->nick, cli->sock, buffer);

        if (buffer[0] == '/') {
            char *cmd_full = strdup(buffer); // strtok modifies the string, so duplicate it
            if (!cmd_full) { perror("strdup"); continue; }

            char *cmd = strtok(buffer + 1, " ");
            if (!cmd) {
                safe_send(cli->sock, "[Server] Unknown command. Type /help.\n");
                free(cmd_full);
                continue;
            }

            if (strcmp(cmd, "nick") == 0) {
                char *newnick = strtok(NULL, " ");
                if (newnick && strlen(newnick) > 0) {
                    Client *existing = find_client_by_nick(newnick);
                    if (existing != NULL && existing != cli) {
                        char error_msg[BUFFER_SIZE];
                        snprintf(error_msg, sizeof(error_msg), "[Server] Nickname '%s' is already taken.\n", newnick);
                        safe_send(cli->sock, error_msg);
                    } else {
                        printf("[INFO] %s changed nickname to %s\n", cli->nick, newnick);
                        strncpy(cli->nick, newnick, sizeof(cli->nick) - 1);
                        cli->nick[sizeof(cli->nick) - 1] = '\0';
                        char success_msg[BUFFER_SIZE];
                        snprintf(success_msg, sizeof(success_msg), "[Server] Nickname updated to %s.\n", cli->nick);
                        safe_send(cli->sock, success_msg);
                    }
                } else {
                    safe_send(cli->sock, "[Server] Usage: /nick <new_nickname>\n");
                }
            } else if (strcmp(cmd, "create") == 0) {
                char *room_name = strtok(NULL, " ");
                cmd_create_room(cli, room_name);
            } else if (strcmp(cmd, "join") == 0) {
                char *room_name = strtok(NULL, " ");
                cmd_join_room(cli, room_name);
            } else if (strcmp(cmd, "leave") == 0) {
                cmd_leave_room(cli);
            } else if (strcmp(cmd, "rooms") == 0) {
                cmd_rooms(cli->sock);
            } else if (strcmp(cmd, "users") == 0) {
                cmd_users(cli);
            } else if (strcmp(cmd, "dm") == 0) {
                 // Use the duplicated cmd_full to extract arguments for /dm
                 char *dm_target_token = strtok(cmd_full + 1, " "); // Skip '/' and "dm"
                 dm_target_token = strtok(NULL, " "); // This should be the target nick
                 char *dm_msg_start = NULL;
                 if (dm_target_token) {
                     dm_msg_start = strtok(NULL, ""); // Get the rest of the string as message
                     // Trim leading spaces from message if any
                     if (dm_msg_start) {
                        while(*dm_msg_start == ' ') dm_msg_start++;
                        if (*dm_msg_start == '\0') dm_msg_start = NULL; // Empty message after spaces
                     }
                 }
                 cmd_dm(cli, dm_target_token, dm_msg_start);
            } else if (strcmp(cmd, "help") == 0) {
                const char *help =
                    "[Server] Available commands:\n"
                    " /nick <name>   - Change your nickname\n"
                    " /rooms         - List available rooms\n"
                    " /create <room_name> - Create a new room and join it\n"
                    " /join <room_id>   - Join an existing room by its ID\n"
                    " /leave         - Leave the current room\n"
                    " /users         - List users (in room if joined, else all connected)\n"
                    " /dm <nick> <msg> - Send a direct message to a user\n"
                    " /help          - Show this help message\n";
                safe_send(cli->sock, help);
            } else {
                safe_send(cli->sock, "[Server] Unknown command. Type /help.\n");
            }
            free(cmd_full);
        } else {
            if (cli->room != NULL) {
                broadcast_room(cli->room, cli, "[%s] %s\n", cli->nick, buffer);
            } else {
                safe_send(cli->sock, "[Server] You must join a room to send messages. Type /rooms or /create.\n");
            }
        }
    }

    printf("[INFO] Cleaning up client %s (fd %d).\n", cli->nick, cli->sock);
    if (cli->room) {
        Room *r = cli->room;
        room_remove_member(r, cli);
        // Notify remaining members *after* removing the client
        // but before potentially destroying the room.
        broadcast_room(r, NULL, "[Server] %s disconnected.\n", cli->nick);
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

    if (!cmd || strlen(cmd) == 0) { /* No action for empty command */ }
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