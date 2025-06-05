# common/Makefile
CC      := gcc
CFLAGS  := -Wall -g -I. 
AR      := ar
ARFLAGS := rcs
TARGET  := libchatprotocol.a

OBJS     := chat_protocol.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

chat_protocol.o: chat_protocol.h
	$(CC) $(CFLAGS) -c chat_protocol.c

clean:
	rm -f $(OBJS) $(TARGET)
