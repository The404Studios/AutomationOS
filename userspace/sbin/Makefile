# Makefile for AutoFS userspace tools

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../../kernel/include
LDFLAGS =

# Targets
TARGETS = mkfs.autofs fsck.autofs

all: $(TARGETS)

mkfs.autofs: mkfs.autofs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built mkfs.autofs"

fsck.autofs: fsck.autofs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built fsck.autofs"

clean:
	rm -f $(TARGETS)

install: $(TARGETS)
	install -m 755 mkfs.autofs /usr/sbin/
	install -m 755 fsck.autofs /usr/sbin/
	@echo "Installed AutoFS tools to /usr/sbin/"

uninstall:
	rm -f /usr/sbin/mkfs.autofs
	rm -f /usr/sbin/fsck.autofs
	@echo "Uninstalled AutoFS tools"

.PHONY: all clean install uninstall
