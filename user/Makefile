# Usermode client (native gcc, -g -O0 for remote-gdbserver debugging).

CC        ?= gcc
SHARED_DIR ?= ../shared
BUILD_DIR ?= build
USER_SRC_DIR ?= $(abspath src)
COMPILE_COMMANDS ?= $(CURDIR)/compile_commands.json
PROJECT_ROOT ?= $(abspath ..)
ROOT_COMPILE_COMMANDS ?= $(PROJECT_ROOT)/compile_commands.json
COMPILE_DB_TOOL ?= $(PROJECT_ROOT)/scripts/compile_commands.py
# The binary name is defined HERE; everything else uses build/current.
BIN       := $(BUILD_DIR)/client
SRC       := $(shell find src -type f -name '*.c' | sort)

CPPFLAGS  ?=
CPPFLAGS  += -I$(USER_SRC_DIR) -I$(SHARED_DIR)
CFLAGS    ?= -g -O0 -Wall -Wextra -std=gnu11

.PHONY: all clean compile-commands

all: $(BIN) compile-commands

$(BIN): $(SRC) $(SHARED_DIR)/shared.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)
	ln -sfn $(notdir $(BIN)) $(BUILD_DIR)/current
	@echo "built $@ (alias $(BUILD_DIR)/current)"

compile-commands:
	@python3 "$(COMPILE_DB_TOOL)" single \
		$(foreach source,$(SRC),--source "$(abspath $(source))") \
		"$(COMPILE_COMMANDS)" "$(CURDIR)" -- \
		"$(CC)" $(CPPFLAGS) $(CFLAGS) -o "$(abspath $(BIN))" "$(abspath $(SRC))"
	@python3 "$(COMPILE_DB_TOOL)" merge --clangd-compatible "$(ROOT_COMPILE_COMMANDS)" \
		"$(PROJECT_ROOT)/kernel/compile_commands.json" "$(COMPILE_COMMANDS)" \
		"$(PROJECT_ROOT)/kernel/linux_compile_commands.json"
	@echo "generated $(COMPILE_COMMANDS)"
	@echo "generated $(ROOT_COMPILE_COMMANDS)"

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
