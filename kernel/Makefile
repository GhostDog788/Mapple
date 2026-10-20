# Build Mapple against an explicitly selected prepared Linux tree.
# The development controller supplies isolated build/output/database paths.
KDIR ?=
MODULE_DIR ?= $(CURDIR)
PROJECT_ROOT := $(abspath $(CURDIR)/..)
INTERMEDIATE_DIR ?=
ARTIFACT_DIR ?=
MODULE_COMPILE_COMMANDS ?=
LINUX_COMPILE_COMMANDS ?=
COMPILE_COMMANDS ?=
KERNEL_SOURCE ?=
COMPILE_DB_TOOL := $(PROJECT_ROOT)/scripts/compile_commands.py

.PHONY: modules prepare-build
modules: prepare-build
	$(MAKE) -C "$(KDIR)" M="$(INTERMEDIATE_DIR)" \
		KCFLAGS="-g -ffile-prefix-map=$(INTERMEDIATE_DIR)=$(MODULE_DIR)" \
		LAB_ROOT_DIR="$(PROJECT_ROOT)" LAB_SHARED_DIR="$(PROJECT_ROOT)/shared" modules
	python3 "$(COMPILE_DB_TOOL)" collect "$(MODULE_COMPILE_COMMANDS)" "$(KDIR)" "$(INTERMEDIATE_DIR)"
	python3 "$(COMPILE_DB_TOOL)" remap "$(MODULE_COMPILE_COMMANDS)" "$(INTERMEDIATE_DIR)" "$(MODULE_DIR)"
	@if [ -n "$(KERNEL_SOURCE)" ] && [ ! -f "$(LINUX_COMPILE_COMMANDS)" ]; then \
		python3 "$(COMPILE_DB_TOOL)" collect "$(LINUX_COMPILE_COMMANDS)" "$(KDIR)" "$(KERNEL_SOURCE)"; \
		python3 "$(COMPILE_DB_TOOL)" kernel-source "$(LINUX_COMPILE_COMMANDS)" "$(MODULE_COMPILE_COMMANDS)" "$(KERNEL_SOURCE)" --arch "$(ARCH)"; \
	elif [ -z "$(KERNEL_SOURCE)" ]; then rm -f "$(LINUX_COMPILE_COMMANDS)"; fi
	python3 "$(COMPILE_DB_TOOL)" merge --clangd-compatible "$(COMPILE_COMMANDS)" "$(MODULE_COMPILE_COMMANDS)" "$(LINUX_COMPILE_COMMANDS)"
	mkdir -p "$(ARTIFACT_DIR)"
	cp "$(INTERMEDIATE_DIR)/mapple.ko" "$(ARTIFACT_DIR)/mapple.ko"

prepare-build:
	@test -n "$(KDIR)" -a -n "$(INTERMEDIATE_DIR)" -a -n "$(ARTIFACT_DIR)" || { echo 'Use make module-build from the repository root.'; exit 1; }
	mkdir -p "$(INTERMEDIATE_DIR)"
	find "$(INTERMEDIATE_DIR)" -type l -delete
	cp -as "$(MODULE_DIR)/src" "$(INTERMEDIATE_DIR)/"
	cp -as "$(MODULE_DIR)/Kbuild" "$(INTERMEDIATE_DIR)/Kbuild"
