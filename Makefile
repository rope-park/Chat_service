# Makefile for chat_client_gtk

# Compiler
CC = gcc

# GTK flags using pkg-config
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

# Target executable name
TARGET = chat_client_gtk

# Source file
SRC = chat_client_gtk.c db_helper.c

# Default target: build the executable
all: $(TARGET)

# Rule to build the executable
$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(GTK_CFLAGS) $(GTK_LIBS) -lsqlite3 -pthread

# Clean target: remove built files
clean:
	rm -f $(TARGET)

