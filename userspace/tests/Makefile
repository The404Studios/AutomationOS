# userspace/tests/Makefile - Build stdio buffering tests

CC = gcc
LD = ld
CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -mno-red-zone -fno-pic -fno-pie -Wno-unused-variable \
         -Wno-implicit-function-declaration -I..
LDFLAGS = -nostdlib -static -n

# Libc object files (link against compiled libc)
LIBC_DIR = ../libc
LIBC_OBJS = $(LIBC_DIR)/stdio.o $(LIBC_DIR)/stdlib.o $(LIBC_DIR)/string.o \
            $(LIBC_DIR)/syscall.o $(LIBC_DIR)/malloc.o $(LIBC_DIR)/errno.o

BUILD_DIR = ../../build/userspace/tests
LINKER_SCRIPT = ../test_program.ld

TESTS = $(BUILD_DIR)/test_stdio_buffering $(BUILD_DIR)/bench_stdio \
        $(BUILD_DIR)/tcache_bench $(BUILD_DIR)/test_tcache_simple \
        $(BUILD_DIR)/test_epoll $(BUILD_DIR)/test_epoll_sockets \
        $(BUILD_DIR)/test_epoll_kernel

.PHONY: all clean

all: $(TESTS)

$(BUILD_DIR)/test_stdio_buffering: test_stdio_buffering.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c test_stdio_buffering.c -o $(BUILD_DIR)/test_stdio_buffering.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/test_stdio_buffering.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/bench_stdio: bench_stdio.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c bench_stdio.c -o $(BUILD_DIR)/bench_stdio.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/bench_stdio.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/tcache_bench: tcache_bench.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c tcache_bench.c -o $(BUILD_DIR)/tcache_bench.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/tcache_bench.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/test_tcache_simple: test_tcache_simple.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c test_tcache_simple.c -o $(BUILD_DIR)/test_tcache_simple.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/test_tcache_simple.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/test_epoll: test_epoll.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c test_epoll.c -o $(BUILD_DIR)/test_epoll.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/test_epoll.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/test_epoll_sockets: test_epoll_sockets.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c test_epoll_sockets.c -o $(BUILD_DIR)/test_epoll_sockets.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/test_epoll_sockets.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

$(BUILD_DIR)/test_epoll_kernel: test_epoll_kernel.c $(LIBC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c test_epoll_kernel.c -o $(BUILD_DIR)/test_epoll_kernel.o
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) $(BUILD_DIR)/test_epoll_kernel.o $(LIBC_OBJS) -o $@
	@echo "Built: $@"

clean:
	rm -f $(BUILD_DIR)/test_stdio_buffering $(BUILD_DIR)/test_stdio_buffering.o
	rm -f $(BUILD_DIR)/bench_stdio $(BUILD_DIR)/bench_stdio.o
	rm -f $(BUILD_DIR)/tcache_bench $(BUILD_DIR)/tcache_bench.o
	rm -f $(BUILD_DIR)/test_tcache_simple $(BUILD_DIR)/test_tcache_simple.o
	rm -f $(BUILD_DIR)/test_epoll $(BUILD_DIR)/test_epoll.o
	rm -f $(BUILD_DIR)/test_epoll_sockets $(BUILD_DIR)/test_epoll_sockets.o
	rm -f $(BUILD_DIR)/test_epoll_kernel $(BUILD_DIR)/test_epoll_kernel.o
