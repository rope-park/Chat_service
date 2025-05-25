# Makefile

CC        = gcc
CFLAGS    = -Wall -Wextra -g
LIBS      = -lsqlite3 -lpthread

# GTK flags
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0)

# ==== CLIENT SIDE ====
CLIENT     = chat_client_gtk
CLIENT_SRC = chat_client_gtk.c
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# ==== SERVER SIDE ====
SERVER     = chat_server
SERVER_SRC = chat_server.c db_helper.c
SERVER_OBJ = $(SERVER_SRC:.c=.o)

.PHONY: all client server clean

# 기본 타깃: 클라이언트와 서버 둘 다 빌드
all: client server

# client 빌드
client: $(CLIENT)

$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(GTK_CFLAGS) $(GTK_LIBS)

# server 빌드
server: $(SERVER)

$(SERVER): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# .c → .o 변환 규칙
%.o: %.c
	$(CC) $(CFLAGS) -c $<

# clean
clean:
	rm -f $(CLIENT) $(SERVER) $(CLIENT_OBJ) $(SERVER_OBJ)
