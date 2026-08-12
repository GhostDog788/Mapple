# Out-of-tree module build against kernel-cache/<target>/. Sources are
# staged into build/intermediate/<target>/<kernel>/ as symlinks (cp -as),
# with -ffile-prefix-map so DWARF paths resolve back here and IDE
# breakpoints bind. Normally invoked by scripts/03-build-module.sh.

HOST_KERNEL    := $(shell uname -r)
CURRENT_CACHE  := $(CURDIR)/kernel-cache/current
CURRENT_TARGET := $(shell if [ -e "$(CURRENT_CACHE)" ]; then basename "$$(readlink -f "$(CURRENT_CACHE)")"; else printf local; fi)
CURRENT_KERNEL := $(shell if [ -f "$(CURRENT_CACHE)/kernel.release" ]; then cat "$(CURRENT_CACHE)/kernel.release"; else printf '$(HOST_KERNEL)'; fi)

KDIR             ?= $(if $(wildcard $(CURRENT_CACHE)/build),$(CURRENT_CACHE)/build,/lib/modules/$(HOST_KERNEL)/build)
BUILD_ID         ?= $(CURRENT_TARGET)/$(CURRENT_KERNEL)
BUILD_ROOT       ?= $(CURDIR)/build
INTERMEDIATE_DIR ?= $(BUILD_ROOT)/intermediate/$(BUILD_ID)
ARTIFACT_DIR     ?= $(BUILD_ROOT)/artifacts/$(BUILD_ID)
MODULE_DIR       ?= $(CURDIR)
PROJECT_ROOT     ?= $(abspath $(CURDIR)/..)
SHARED_DIR       ?= $(abspath $(CURDIR)/../shared)
MODULE_COMPILE_COMMANDS ?= $(MODULE_DIR)/module_compile_commands.json
COMPILE_COMMANDS ?= $(MODULE_DIR)/compile_commands.json
LINUX_COMPILE_COMMANDS ?= $(MODULE_DIR)/linux_compile_commands.json
ROOT_COMPILE_COMMANDS ?= $(PROJECT_ROOT)/compile_commands.json
COMPILE_DB_TOOL  ?= $(PROJECT_ROOT)/scripts/compile_commands.py
KERNEL_DB_TOOL   ?= $(KDIR)/scripts/clang-tools/gen_compile_commands.py
KERNEL_SOURCE    ?= $(realpath $(CURRENT_CACHE)/source)

# Only these are staged (staging the whole dir would recurse into build/).
STAGE_ITEMS ?= src Kbuild

EXTRA_CCFLAGS  ?= -g -DDEBUG
KCFLAGS_INJECT := -ffile-prefix-map=$(INTERMEDIATE_DIR)=$(MODULE_DIR) $(EXTRA_CCFLAGS)

.PHONY: all modules prepare-build clean help

all: modules

help:
	@echo "Targets: modules (default), clean, prepare-build (internal)"
	@echo "Normally invoked by scripts/03-build-module.sh <target>."
	@echo "  KDIR              $(KDIR)"
	@echo "  BUILD_ID          $(BUILD_ID)"
	@echo "  INTERMEDIATE_DIR  $(INTERMEDIATE_DIR)"
	@echo "  ARTIFACT_DIR      $(ARTIFACT_DIR)"

modules: prepare-build
	$(MAKE) -C "$(KDIR)" M="$(INTERMEDIATE_DIR)" \
		KCFLAGS="$(KCFLAGS_INJECT)" LAB_ROOT_DIR="$(PROJECT_ROOT)" \
		LAB_SHARED_DIR="$(SHARED_DIR)" modules
	@python3 "$(KERNEL_DB_TOOL)" -d "$(KDIR)" -o "$(MODULE_COMPILE_COMMANDS)" "$(INTERMEDIATE_DIR)"
	@python3 "$(COMPILE_DB_TOOL)" remap "$(MODULE_COMPILE_COMMANDS)" "$(INTERMEDIATE_DIR)" "$(MODULE_DIR)"
	@if [ -n "$(KERNEL_SOURCE)" ]; then \
		python3 "$(KERNEL_DB_TOOL)" -d "$(KERNEL_SOURCE)" \
			-o "$(LINUX_COMPILE_COMMANDS)" "$(KERNEL_SOURCE)"; \
	fi
	@python3 "$(COMPILE_DB_TOOL)" merge --clangd-compatible "$(COMPILE_COMMANDS)" \
		"$(MODULE_COMPILE_COMMANDS)" "$(PROJECT_ROOT)/user/compile_commands.json" \
		"$(LINUX_COMPILE_COMMANDS)"
	@python3 "$(COMPILE_DB_TOOL)" merge --clangd-compatible "$(ROOT_COMPILE_COMMANDS)" \
		"$(COMPILE_COMMANDS)"
	@echo "generated $(COMPILE_COMMANDS)"
	@echo "generated $(ROOT_COMPILE_COMMANDS)"
	@mkdir -p "$(ARTIFACT_DIR)"
	@set -e; \
	kos=$$(find "$(INTERMEDIATE_DIR)" -maxdepth 2 -name '*.ko' -type f); \
	if [ -z "$$kos" ]; then \
		echo "Makefile: no .ko produced under $(INTERMEDIATE_DIR);" >&2; \
		echo "  check the Kbuild output above for compile errors." >&2; \
		exit 1; \
	fi; \
	for ko in $$kos; do \
		name=$$(basename "$$ko"); \
		mv "$$ko" "$(ARTIFACT_DIR)/$$name"; \
		echo "built $(ARTIFACT_DIR)/$$name"; \
	done

# Wipe symlink scaffolding first so removed sources don't linger stale.
prepare-build:
	@if [ ! -e "$(MODULE_DIR)/Kbuild" ]; then \
		echo "Makefile: no Kbuild under $(MODULE_DIR)" >&2; \
		exit 1; \
	fi
	@cp --help 2>&1 | grep -q -- '--symbolic-link' || { \
		echo "Makefile: GNU 'cp' (with --symbolic-link, --archive) is required;" >&2; \
		echo "  the lab targets Debian-based hosts. On macOS: brew install coreutils, then re-run with CP=gcp." >&2; \
		exit 1; \
	}
	@mkdir -p "$(INTERMEDIATE_DIR)"
	@find "$(INTERMEDIATE_DIR)" -depth -type l -delete
	@find "$(INTERMEDIATE_DIR)" -depth -type d -empty -not -path "$(INTERMEDIATE_DIR)" -delete
	@for item in $(STAGE_ITEMS); do \
		[ -e "$(MODULE_DIR)/$$item" ] && cp -as "$(MODULE_DIR)/$$item" "$(INTERMEDIATE_DIR)/" || true; \
	done

clean:
	@if [ -d "$(INTERMEDIATE_DIR)" ] && [ -e "$(INTERMEDIATE_DIR)/Kbuild" ]; then \
		$(MAKE) -C "$(KDIR)" M="$(INTERMEDIATE_DIR)" clean || true; \
	fi
	rm -rf "$(INTERMEDIATE_DIR)" "$(ARTIFACT_DIR)"
