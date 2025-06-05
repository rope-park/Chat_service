# client/Makefile

CC      := gcc
CFLAGS  := -Wall -g -I../common
LDFLAGS := ../common/libchatprotocol.a -lpthread

SRCS    := chat_client.c
OBJS    := $(SRCS:.c=.o)
TARGET  := chat_client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

chat_client.o: chat_client.c chat_client.h ../common/chat_protocol.h
	$(CC) $(CFLAGS) -c chat_client.c

clean:
	rm -f $(OBJS) $(TARGET)
