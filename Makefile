# chat_project/Makefile (최상위)

CC      := gcc
CFLAGS  := -Wall -g -I./common
LDFLAGS := -lpthread -lsqlite3

# 서버 생성
SERVER_DIR   := server
SERVER_OBJS  := $(SERVER_DIR)/chat_server.o $(SERVER_DIR)/db_helper.o
SERVER_TGT   := $(SERVER_DIR)/chat_server

# 콘솔 클라이언트 생성
CLIENT_DIR   := client
CLIENT_OBJS  := $(CLIENT_DIR)/chat_client.o
CLIENT_TGT   := $(CLIENT_DIR)/chat_client

# GTK 클라이언트 생성 (예: gtk3)
GTK_OBJS     := chat_client_gtk.o
GTK_TGT      := chat_client_gtk
GTK_CFLAGS   := $(CFLAGS) `pkg-config --cflags gtk+-3.0`
GTK_LDFLAGS  := `pkg-config --libs gtk+-3.0` -lpthread

all: server client gtk_client

# 1) server 빌드
server: $(SERVER_TGT)

$(SERVER_TGT): $(SERVER_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# 서버 오브젝트 생성
$(SERVER_DIR)/chat_server.o: $(SERVER_DIR)/chat_server.c $(SERVER_DIR)/chat_server.h common/chat_protocol.h $(SERVER_DIR)/db_helper.h
	$(CC) $(CFLAGS) -c $< -o $@

$(SERVER_DIR)/db_helper.o: $(SERVER_DIR)/db_helper.c $(SERVER_DIR)/db_helper.h common/chat_protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# 2) client 빌드 (콘솔)
client: $(CLIENT_TGT)

$(CLIENT_TGT): $(CLIENT_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(CLIENT_DIR)/chat_client.o: $(CLIENT_DIR)/chat_client.c $(CLIENT_DIR)/chat_client.h common/chat_protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# 3) GTK 버전 클라이언트 빌드
gtk_client: $(GTK_TGT)

$(GTK_TGT): $(GTK_OBJS)
	$(CC) -o $@ $^ $(GTK_LDFLAGS)

chat_client_gtk.o: chat_client_gtk.c chat_client_gtk.h common/chat_protocol.h
	$(CC) $(GTK_CFLAGS) -c chat_client_gtk.c -o chat_client_gtk.o

clean:
	rm -f $(SERVER_OBJS) $(SERVER_TGT) \
	      $(CLIENT_OBJS) $(CLIENT_TGT) \
	      $(GTK_OBJS)    $(GTK_TGT)
