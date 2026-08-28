.PHONY: all kernel clean clean-kernel help

all: kernel

kernel:
	$(MAKE) -C kernel modules

clean: clean-kernel

clean-kernel:
	$(MAKE) -C kernel clean

help:
	@echo "Targets: all (default), kernel, clean, clean-kernel"
	@echo "Kernel build+deploy+debug:  kernel/scripts/0*.sh <target> <kgdb|qemu>"
