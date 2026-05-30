# Benchmark Suite Makefile
# Builds userspace benchmarks for AutomationOS

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../../kernel/include -static
LDFLAGS = -static

BENCHMARKS = \
	bench_context_switch \
	bench_syscall \
	bench_futex \
	bench_file_io \
	bench_network \
	bench_fork \
	bench_malloc \
	bench_epoll \
	bench_memory \
	bench_huge_pages \
	bench_readahead_sendfile

all: $(BENCHMARKS)

bench_context_switch: bench_context_switch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_syscall: bench_syscall.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_futex: bench_futex.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_file_io: bench_file_io.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_network: bench_network.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_fork: bench_fork.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_malloc: bench_malloc.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_epoll: bench_epoll.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_memory: bench_memory.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_huge_pages: bench_huge_pages.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench_readahead_sendfile: bench_readahead_sendfile.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Run all benchmarks
test: all
	@echo "Running benchmark suite..."
	@for bench in $(BENCHMARKS); do \
		echo ""; \
		echo "=== Running $$bench ==="; \
		./$$bench || true; \
	done

clean:
	rm -f $(BENCHMARKS) *.o

install: all
	@echo "Installing benchmarks to ISO build..."
	@mkdir -p ../../iso_root/tests/bench
	@cp $(BENCHMARKS) ../../iso_root/tests/bench/
	@echo "Benchmarks installed"

.PHONY: all test clean install
