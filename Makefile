SHELL := /bin/bash
.DEFAULT_GOAL := help
LAB := python3 scripts/run_lab.py
ifeq ($(origin GOTOOLCHAIN),undefined)
ifneq ($(HELP),1)
ifneq ($(filter api api-verify test check,$(MAKECMDGOALS)),)
GOTOOLCHAIN := $(shell python3 scripts/go_toolchain.py)
endif
endif
endif
export GOTOOLCHAIN
# Explicit input data, never interpolated into shell commands.
export NAME MACHINE KERNEL HOST SSH_USER SSH_PORT SSH_KEY ASK_PASS GDB_ENDPOINT
export VERSION ARCH LINUX_ARCH DEFCONFIG IMAGE_PATH TOOLCHAIN BTF PROFILE CONFIG PATCHES BUILD SOURCE VMLINUX DEBUG_INFO BUILD_ID SYSTEM_MAP IMAGE MODULES
export CROSS_COMPILE LLVM INSTALLER FETCHER SIGNER TOOLCHAIN_PACKAGES
export JOBS MODULE_PORT MODULE_PARAMS MEMORY CPUS DISK TERMINAL FOREGROUND NO_INSTALL
export OFFLINE CASCADE RUN MIN_DEBUG GDB NO_OPEN WINDOW
export CLANGD NAV_TIMEOUT
export HELP COMMAND
ifneq ($(filter command line environment environment override,$(origin CC)),)
export CC
else
unexport CC
endif
COMMANDS := setup kernel-create kernel-import kernel-enrich kernel-build kernel-list kernel-show kernel-remove \
 machine-create machine-add machine-list machine-show machine-configure machine-inspect machine-use machine-adopt machine-prepare \
 machine-start machine-stop machine-ssh machine-remove machine-destroy module-build module-run module-debug \
 server-build server-run tui-build tui-run gdb-open gdb-attach vscode-open debug-list doctor clean \
 navigation-prepare navigation-status navigation-setup test-debugging
UTILITY_COMMANDS := check test test-tools test-navigation api api-verify
.PHONY: $(COMMANDS) help $(UTILITY_COMMANDS)
ifeq ($(HELP),1)
# Help must bypass prerequisites too: `make check HELP=1` never runs checks.
$(COMMANDS) $(UTILITY_COMMANDS):
	@$(LAB) help $@
else
$(COMMANDS):
	@$(LAB) $@
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
endif
help:
	@$(LAB) help
