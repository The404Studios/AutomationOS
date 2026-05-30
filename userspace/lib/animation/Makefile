# AutomationOS Animation Library Makefile

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -O2 -fPIC -I.
LDFLAGS = -lm

# Source files
SRCS = animator.c \
       animator_control.c \
       ui_animations.c

# Object files
OBJS = $(SRCS:.c=.o)

# Library output
LIB = libanim.a
SHARED_LIB = libanim.so

# Build targets
all: $(LIB) $(SHARED_LIB)

# Static library
$(LIB): $(OBJS)
	$(AR) rcs $@ $^
	@echo "Built static library: $(LIB)"

# Shared library
$(SHARED_LIB): $(OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built shared library: $(SHARED_LIB)"

# Compile object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJS) $(LIB) $(SHARED_LIB)
	@echo "Cleaned animation library"

# Install library
install: $(LIB) $(SHARED_LIB)
	mkdir -p /usr/local/lib
	mkdir -p /usr/local/include/automationos
	cp $(LIB) /usr/local/lib/
	cp $(SHARED_LIB) /usr/local/lib/
	cp animator.h /usr/local/include/automationos/
	cp ui_animations.h /usr/local/include/automationos/
	cp window_animations.h /usr/local/include/automationos/
	ldconfig
	@echo "Installed animation library"

# Uninstall
uninstall:
	rm -f /usr/local/lib/$(LIB)
	rm -f /usr/local/lib/$(SHARED_LIB)
	rm -f /usr/local/include/automationos/animator.h
	rm -f /usr/local/include/automationos/ui_animations.h
	rm -f /usr/local/include/automationos/window_animations.h
	ldconfig
	@echo "Uninstalled animation library"

# Build example
example: example_demo.c $(LIB)
	$(CC) $(CFLAGS) -o animation_demo example_demo.c $(LIB) $(LDFLAGS)
	@echo "Built animation demo"

.PHONY: all clean install uninstall example
