# userspace/system/services/Makefile - Build service manager

CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -pthread
LDFLAGS = -pthread

BUILD_DIR = ../../../build/userspace/system/services

all: $(BUILD_DIR)/servicemanager

$(BUILD_DIR)/servicemanager.o: servicemanager.c servicemanager.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c servicemanager.c -o $@

$(BUILD_DIR)/servicemanager_main.o: servicemanager_main.c servicemanager.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c servicemanager_main.c -o $@

$(BUILD_DIR)/servicemanager: $(BUILD_DIR)/servicemanager.o $(BUILD_DIR)/servicemanager_main.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
