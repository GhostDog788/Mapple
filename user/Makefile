# Usermode client (native gcc, -g -O0 for remote-gdbserver debugging).

CC        ?= gcc
SHARED_DIR ?= ../shared
BUILD_DIR ?= build
# The binary name is defined HERE; everything else uses build/current.
BIN       := $(BUILD_DIR)/client
SRC       := src/main.c

CFLAGS    ?= -g -O0 -Wall -Wextra -std=gnu11 -I$(SHARED_DIR)

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) $(SHARED_DIR)/shared.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC)
	ln -sfn $(notdir $(BIN)) $(BUILD_DIR)/current
	@echo "built $@ (alias $(BUILD_DIR)/current)"

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
