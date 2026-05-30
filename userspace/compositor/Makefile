# userspace/compositor/Makefile - Build integrated compositor with framebuffer + input

CC = gcc
LD = ld

# For hosted environment (testing on Linux/WSL)
HOSTED_CFLAGS = -Wall -Wextra -O2 -std=gnu11 -g
HOSTED_LDFLAGS = -lm

# For freestanding environment (bare metal)
FREESTANDING_CFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
                      -fno-pic -fno-pie -mno-red-zone -Wall -Wextra -O2 -std=gnu11
FREESTANDING_LDFLAGS = -nostdlib -static -n -no-pie

BUILD_DIR = ../../build/userspace/compositor

# Source files for compositor
COMPOSITOR_SRCS = main.c fb.c render.c input.c
COMPOSITOR_OBJS = $(COMPOSITOR_SRCS:.c=.o)

# Integration test
TEST_SRCS = test_integration.c fb.c render.c input.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

all: hosted

# Hosted build (for Linux/WSL testing)
hosted: $(BUILD_DIR)/compositor-hosted $(BUILD_DIR)/test-integration

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/compositor-hosted: $(COMPOSITOR_SRCS) | $(BUILD_DIR)
	$(CC) $(HOSTED_CFLAGS) $(COMPOSITOR_SRCS) -o $@ $(HOSTED_LDFLAGS)
	@echo "Built hosted compositor: $@"

$(BUILD_DIR)/test-integration: $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(HOSTED_CFLAGS) $(TEST_SRCS) -o $@ $(HOSTED_LDFLAGS)
	@echo "Built integration test: $@"

# Freestanding build (for bare metal)
freestanding: $(BUILD_DIR)/compositor

$(BUILD_DIR)/compositor: $(COMPOSITOR_SRCS) | $(BUILD_DIR)
	$(CC) $(FREESTANDING_CFLAGS) -c main.c -o main.o
	$(CC) $(FREESTANDING_CFLAGS) -c fb.c -o fb.o
	$(CC) $(FREESTANDING_CFLAGS) -c render.c -o render.o
	$(CC) $(FREESTANDING_CFLAGS) -c input.c -o input.o
	$(LD) $(FREESTANDING_LDFLAGS) -e main -T../userspace.ld \
	      main.o fb.o render.o input.o -o $@
	@echo "Built freestanding compositor: $@"

clean:
	rm -f *.o
	rm -f $(BUILD_DIR)/compositor-hosted
	rm -f $(BUILD_DIR)/test-integration
	rm -f $(BUILD_DIR)/compositor

.PHONY: all hosted freestanding clean
