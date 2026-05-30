# Makefile for audit userspace tools

# Compiler settings
CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=c11 -O2 -I../../kernel/include
LDFLAGS :=

# Target binaries
BINARIES := auditctl ausearch aureport

# Build rules
all: $(BINARIES)

auditctl: auditctl.c
	@echo "[CC] $@"
	@$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

ausearch: ausearch.c
	@echo "[CC] $@"
	@$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

aureport: aureport.c
	@echo "[CC] $@"
	@$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Install
install: $(BINARIES)
	@echo "[INSTALL] audit tools"
	@mkdir -p /usr/local/bin
	@cp $(BINARIES) /usr/local/bin/
	@chmod +x /usr/local/bin/auditctl
	@chmod +x /usr/local/bin/ausearch
	@chmod +x /usr/local/bin/aureport

# Clean
clean:
	@echo "[CLEAN] audit tools"
	@rm -f $(BINARIES) *.o

# Rebuild
rebuild: clean all

.PHONY: all install clean rebuild
