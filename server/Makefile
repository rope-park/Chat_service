# server/Makefile

CC      := gcc
CFLAGS  := -Wall -g -I../common
LDFLAGS := ../common/libchatprotocol.a -lpthread -lsqlite3

SRCS    := chat_server.c db_helper.c
OBJS    := $(SRCS:.c=.o)
TARGET  := chat_server

all: $(TARGET)

# 1) 서버 바이너리 링크
$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# 2) .c → .o 컴파일
chat_server.o: chat_server.c chat_server.h db_helper.h ../common/chat_protocol.h
	$(CC) $(CFLAGS) -c chat_server.c

db_helper.o: db_helper.c db_helper.h chat_server.h ../common/chat_protocol.h
	$(CC) $(CFLAGS) -c db_helper.c

run:
	CHAT_DB_FILE=/home/ropepark/Chat_service/my_chat.db ./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
