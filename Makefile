SHELL := /bin/bash
.DEFAULT_GOAL := help
LAB := python3 scripts/lab.py
ifeq ($(origin GOTOOLCHAIN),undefined)
GOTOOLCHAIN := $(shell python3 scripts/go_toolchain.py)
endif
export GOTOOLCHAIN
# Explicit input data, never interpolated into shell commands.
export NAME MACHINE KERNEL HOST SSH_USER SSH_PORT SSH_KEY ASK_PASS GDB_ENDPOINT
export VERSION ARCH LINUX_ARCH DEFCONFIG IMAGE_PATH TOOLCHAIN BTF PROFILE CONFIG PATCHES BUILD SOURCE VMLINUX DEBUG_INFO BUILD_ID SYSTEM_MAP IMAGE MODULES
export CROSS_COMPILE LLVM INSTALLER FETCHER SIGNER TOOLCHAIN_PACKAGES
export JOBS MODULE_PORT MODULE_PARAMS MEMORY CPUS DISK TERMINAL FOREGROUND NO_INSTALL
export OFFLINE CASCADE RUN MIN_DEBUG GDB NO_OPEN
export CLANGD NAV_TIMEOUT
ifneq ($(filter command line environment environment override,$(origin CC)),)
export CC
else
unexport CC
endif
COMMANDS := setup kernel-create kernel-import kernel-enrich kernel-build kernel-list kernel-show kernel-remove \
 machine-create machine-add machine-list machine-show machine-configure machine-inspect machine-use machine-adopt machine-prepare \
 machine-start machine-stop machine-ssh machine-remove machine-destroy module-build module-run module-debug \
 server-build server-run tui-build tui-run gdb-open gdb-attach vscode-open debug-list doctor clean \
 navigation-prepare navigation-status navigation-setup
.PHONY: $(COMMANDS) help check test test-tools test-navigation api api-verify
$(COMMANDS):
	$(LAB) $@
api:
	cd user/api && go generate ./...
api-verify:
	python3 scripts/api_verify.py
test:
	cd user && go test ./...
test-tools:
	python3 -m unittest discover -s scripts/tests -v
test-navigation:
	python3 scripts/tests/navigation_integration.py
check: api-verify test-tools
	cd user && go build ./... && go vet ./... && go test ./...
	python3 -m compileall -q scripts
help:
	@echo 'Host:       make setup [NO_INSTALL=1] [TOOLCHAIN_PACKAGES="gcc-aarch64-linux-gnu"]'
	@echo 'Kernels:    kernel-create NAME=... VERSION=...; kernel-build KERNEL=...'
	@echo '            kernel-import NAME=... BUILD=... [SOURCE=... VMLINUX=...]'
	@echo '            kernel-enrich KERNEL=...; kernel-list; kernel-show; kernel-remove [CASCADE=1]'
	@echo 'Machines:   machine-create NAME=... KERNEL=... (Ubuntu QEMU); machine-start'
	@echo '            machine-add NAME=... HOST=... SSH_USER=... [SSH_KEY=... GDB_ENDPOINT=...]'
	@echo '            machine-inspect; machine-adopt [OFFLINE=1]; machine-prepare KERNEL=...'
	@echo '            machine-configure [GDB_ENDPOINT=... INSTALLER=...] updates connection/debug settings'
	@echo '            machine-list; machine-show; machine-use NAME=...; machine-stop; machine-ssh'
	@echo '            machine-remove (record only); machine-destroy (owned QEMU disk + record)'
	@echo 'Build/run:  module-build [KERNEL=...]; module-run; server-build; server-run; tui-build; tui-run'
	@echo 'Debug:      module-debug [MIN_DEBUG=module|kernel-symbol|kernel-source]; gdb-open; vscode-open'
	@echo '            debug-list; gdb-attach RUN=... (current terminal)'
	@echo 'Selection:  MACHINE=... overrides the active machine for one command; no active kernel selection'
	@echo 'Navigation: navigation-prepare [KERNEL=...] (offline); navigation-status; navigation-setup (VS Code extension)'
	@echo 'Checks:     doctor; check; test; test-tools; api-verify. Cleanup: clean (application builds only)'
	@echo 'Reference:  docs/guides/setup.md and docs/guides/usage.md'
