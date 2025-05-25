# Makefile

# Compiler
CC = gcc
CFLAGS: = -Wall -Wextra -g
LIBS := -lsqlite3 -lpthread

# GTK flags using pkg-config
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

# ==== CLIENT SIDE ====
CLIENT := chat_client_gtk
CLIENT_SRC := chat_client_gtk.c
CLIENT_OBJ := $(CLIENT_SRC:.c=.o)

# ==== SERVER SIDE ====
SERVER := chat_server
SERVER_SRC := chat_server.c db_helper.c
SERVER_OBJ := $(SERVER_SRC:.c=.o)

.PHONY: all client server clean

# Default target
client: $(CLIENT)

# Build targets
$(CLIENT): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(GTK_CFLAGS) $(GTK_LIBS)

# Build server
server: $(SERVER)

# Build targets
$(SERVER): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c chat_server.h db_helper.h
	$(CC) $(CFLAGS) -c $<

# clean target: remove compiled files
clean:
	rm -f $(CLIENT) $(SERVER) *.o
	rm -f $(TARGET)

