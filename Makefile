# Dispatch: kernel/ (out-of-tree module), user/ (client). Shared ABI in
# shared/shared.h. Per-target workflows live in kernel/scripts/ and user/scripts/.

.PHONY: all kernel user clean clean-kernel clean-user help

all: kernel user

kernel:
	$(MAKE) -C kernel modules

user:
	$(MAKE) -C user

clean: clean-kernel clean-user

clean-kernel:
	$(MAKE) -C kernel clean

clean-user:
	$(MAKE) -C user clean

help:
	@echo "Targets: all (default), kernel, user, clean, clean-kernel, clean-user"
	@echo "Kernel build+deploy+debug:  kernel/scripts/0*.sh <target> <kgdb|qemu>"
	@echo "Usermode deploy+gdbserver:  user/scripts/deploy-and-debug.sh <target> [args...]"
