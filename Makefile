.PHONY: all kernel user clean clean-kernel clean-user help

all: kernel user

kernel:
	$(MAKE) -C kernel modules

user:
	cd user && go build ./... && go vet ./... && go test ./...

clean: clean-kernel clean-user

clean-kernel:
	$(MAKE) -C kernel clean

clean-user:
	cd user && go clean ./...

help:
	@echo "Targets: all (default), kernel, user, clean, clean-kernel, clean-user"
	@echo "Kernel build+deploy+debug:  kernel/scripts/0*.sh <target> <kgdb|qemu>"
