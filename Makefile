.PHONY: all kernel user api-verify clean clean-kernel clean-user help

all: kernel user

kernel:
	$(MAKE) -C kernel modules

user: api-verify
	cd user && go build ./... && go vet ./... && go test ./...
	cd user && CGO_ENABLED=0 go build -o bin/mappled ./cmd/mappled
	cd user && CGO_ENABLED=0 go build -o bin/mapple ./cmd/mapple

# Regenerates api/*.gen.go from openapi.yaml and fails if the committed,
# checked-in output would change -- catches a hand-edited generated file or
# an openapi.yaml edit that nobody regenerated for.
api-verify:
	cd user/api && go generate ./...
	git diff --exit-code -- user/api/server.gen.go

clean: clean-kernel clean-user

clean-kernel:
	$(MAKE) -C kernel clean

clean-user:
	cd user && go clean ./...
	rm -rf user/bin

help:
	@echo "Targets: all (default), kernel, user, api-verify, clean, clean-kernel, clean-user"
	@echo "Kernel build+deploy+debug:  kernel/scripts/0*.sh <target> <kgdb|qemu>"
