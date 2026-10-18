SHELL := /bin/bash
.DEFAULT_GOAL := all
LAB := python3 scripts/lab.py

ifeq ($(origin GOTOOLCHAIN),undefined)
GOTOOLCHAIN := $(shell python3 scripts/go_toolchain.py)
endif
export GOTOOLCHAIN

# Pass options as environment data; .lab/active persists the selected VM.
export NAME HOST SSH_USER SSH_PORT SSH_KEY ASK_PASS GDB_ENDPOINT SOURCE VMLINUX
export MODULE_PORT MODULE_PARAMS MEMORY CPUS DISK TERMINAL FOREGROUND NO_INSTALL
export JOBS

.PHONY: all setup debug-kernel help kernel server tui user build-kernel build-server build-tui \
 run-server run-tui run-kernel run-kernel-debug gdb gdb-here doctor \
 vm-create vm-add vm-list vm-use vm-up vm-down vm-status vm-ssh vm-setup vm-sync \
 dev-config api api-verify check test test-tools clean clean-kernel clean-user

all: kernel user
debug-kernel:
	$(LAB) debug-kernel
setup:
	$(LAB) setup

vm-create vm-add vm-list vm-use vm-up vm-down vm-status vm-ssh vm-setup vm-sync doctor:
	$(LAB) $@

kernel build-kernel:
	$(LAB) kernel-build

server build-server:
	cd user && CGO_ENABLED=0 go build -o bin/mappled ./cmd/mappled

tui build-tui:
	cd user && CGO_ENABLED=0 go build -o bin/mapple ./cmd/mapple

user: server tui

dev-config:
	$(LAB) dev-config

run-server: server dev-config
	./user/bin/mappled -config .lab/dev/mappled.json

run-tui: tui dev-config
	$(LAB) tui

# Each upload first builds incrementally for the persistent active slot.
run-kernel:
	$(LAB) kernel-run

run-kernel-debug:
	$(LAB) kernel-debug

# Connect to an already prepared run. VS Code F5 prepares its own run.
gdb gdb-here:
	$(LAB) $@

api:
	cd user/api && go generate ./...

api-verify:
	python3 scripts/api_verify.py

test:
	cd user && go test ./...

test-tools:
	python3 -m unittest discover -s scripts/tests -v

check: api-verify test-tools
	cd user && go build ./... && go vet ./... && go test ./...
	@find kernel/scripts user/scripts -name '*.sh' -print0 | xargs -0 -r -n 1 bash -n

clean: clean-kernel clean-user
clean-kernel:
	$(LAB) kernel-clean
clean-user:
	cd user && go clean ./...
	rm -rf user/bin

help:
	@echo 'First clone:     make setup'
	@echo 'New Ubuntu VM:   make vm-create NAME=dev; make vm-use NAME=dev; make vm-up; make vm-setup'
	@echo 'Existing VM:     make vm-add NAME=dev HOST=... SSH_USER=... GDB_ENDPOINT=127.0.0.1:1234'
	@echo '                 optional: SSH_KEY=... ASK_PASS=1 SOURCE=... VMLINUX=...'
	@echo 'VM slots:        make vm-list | vm-use NAME=... | vm-status | vm-ssh | vm-sync'
	@echo 'VM lifecycle:    make vm-up | vm-down (repo-created VMs); vm-setup provisions + reboots guest'
	@echo 'Debug kernel:    vm-setup builds/installs Linux 6.6.6 once, or reuses SOURCE=... VMLINUX=...'
	@echo '                 make debug-kernel builds it separately; JOBS=8 controls parallelism'
	@echo 'Build:           make kernel | server | tui | user | all'
	@echo 'Run:             make run-server | run-tui | run-kernel | run-kernel-debug'
	@echo 'Kernel debugger: make gdb (new terminal), make gdb-here, or VS Code F5'
	@echo 'Validate:        make doctor | check | test | api-verify'
	@echo 'Terminal:        FOREGROUND=1 uses this terminal; TERMINAL="xterm -e" overrides the launcher'
	@echo 'Details:         docs/development.md'
