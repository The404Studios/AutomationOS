# userspace/Makefile - Build all userspace components

CC = gcc
LD = ld
CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector -mno-red-zone -fno-pic -fno-pie -Wno-unused-variable -Wno-implicit-function-declaration
LDFLAGS = -nostdlib -static -n

# Standalone test programs (no libc dependencies)
TEST_PROGRAMS = ../build/userspace/tests/test_minimal \
                ../build/userspace/tests/bench_batch

.PHONY: all libc libs init shell compositor wm apps llm tests clean

all: libc libs init shell compositor wm apps llm tests

# Build minimal test program outside source directories.
../build/userspace/tests/test_minimal: test_minimal.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c test_minimal.c -o $(@D)/test_minimal.o
	$(LD) $(LDFLAGS) -T test_program.ld $(@D)/test_minimal.o -o $@
	@echo "Built minimal userspace test: $@"

# Build batch syscall benchmark
../build/userspace/tests/bench_batch: bench_batch.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c bench_batch.c -o $(@D)/bench_batch.o
	$(LD) $(LDFLAGS) -T test_program.ld $(@D)/bench_batch.o -o $@
	@echo "Built batch benchmark: $@"

tests: $(TEST_PROGRAMS)

libc:
	$(MAKE) -C libc/

# Build userspace libraries (font, image, IPC)
libs: libc
	@echo "Building userspace libraries..."
	@if [ -f lib/font/Makefile ]; then $(MAKE) -C lib/font/ 2>/dev/null || true; fi
	@echo "Libraries built."

init: libc
	$(MAKE) -C init/

shell: libc
	$(MAKE) -C shell/

compositor: libc libs
	$(MAKE) -C compositor/ all

wm: libc libs
	$(MAKE) -C wm/

llm: libc
	$(MAKE) -C llm/

apps: libc libs
	$(MAKE) -C apps/

clean:
	$(MAKE) -C libc/ clean
	@if [ -f lib/font/Makefile ]; then $(MAKE) -C lib/font/ clean 2>/dev/null || true; fi
	$(MAKE) -C init/ clean
	$(MAKE) -C shell/ clean
	$(MAKE) -C compositor/ clean-freestanding 2>/dev/null || true
	$(MAKE) -C wm/ clean 2>/dev/null || true
	$(MAKE) -C apps/ clean 2>/dev/null || true
	rm -f $(TEST_PROGRAMS) ../build/userspace/tests/test_minimal.o
