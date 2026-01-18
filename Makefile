CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
LDLIBS ?= -levent

SRC_DIR := src
BIN_DIR := bin

SERVER_SRC := $(SRC_DIR)/server.c
CLIENT_SRC := $(SRC_DIR)/client.c

SERVER_BIN := $(BIN_DIR)/server
CLIENT_BIN := $(BIN_DIR)/client

.PHONY: all clean

all: $(SERVER_BIN) $(CLIENT_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SERVER_BIN): $(SERVER_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT_BIN): $(CLIENT_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(BIN_DIR) *.o *.d
