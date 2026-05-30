# KTest Framework Makefile
# Builds the in-kernel testing framework and all test suites

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
CFLAGS += -ffreestanding -fno-stack-protector -fno-pic
CFLAGS += -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2
CFLAGS += -I../../include

# Test framework sources
KTEST_SRC = ktest.c
KTEST_OBJ = ktest.o

# Test files
TEST_SOURCES = $(wildcard tests/test_*.c)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)

# All objects
ALL_OBJECTS = $(KTEST_OBJ) $(TEST_OBJECTS)

.PHONY: all clean

all: $(ALL_OBJECTS)

# Build test framework
$(KTEST_OBJ): $(KTEST_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Build test files
tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ALL_OBJECTS)
	rm -f *.o tests/*.o

# Show test file information
info:
	@echo "Test Framework Objects:"
	@echo "  $(KTEST_OBJ)"
	@echo ""
	@echo "Test Suite Objects:"
	@for obj in $(TEST_OBJECTS); do echo "  $$obj"; done
	@echo ""
	@echo "Total: $(words $(ALL_OBJECTS)) files"

# Verify all tests compile
verify: all
	@echo "All tests compiled successfully!"
