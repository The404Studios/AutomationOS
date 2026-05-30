# Makefile for audit logging subsystem

# Compiler settings
CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=c11 -O2 -fno-stack-protector \
          -fno-builtin -nostdlib -nostdinc -mno-red-zone \
          -I../include -I../../include

# Source files
SRCS := buffer.c log.c filter.c rules.c
OBJS := $(SRCS:.c=.o)

# Target library
LIB := audit.a

# Build rules
all: $(LIB)

$(LIB): $(OBJS)
	@echo "[AR] $@"
	@ar rcs $@ $^

%.o: %.c
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Dependencies
buffer.o: buffer.c ../include/audit.h ../include/kernel.h ../include/mem.h
log.o: log.c ../include/audit.h ../include/kernel.h ../include/mem.h ../include/sched.h
filter.o: filter.c ../include/audit.h ../include/kernel.h
rules.o: rules.c ../include/audit.h ../include/kernel.h ../include/mem.h

# Clean
clean:
	@echo "[CLEAN] audit"
	@rm -f $(OBJS) $(LIB)

# Rebuild
rebuild: clean all

.PHONY: all clean rebuild
