# Theme Engine Makefile

CC = x86_64-elf-gcc
AR = x86_64-elf-ar
CFLAGS = -O2 -Wall -Wextra -I../../shell/theme -I../..
LDFLAGS = -lm

# Source files
SRCS = theme.c parser.c
OBJS = $(SRCS:.c=.o)

# Output library
LIB = libtheme.a

# Test executable
TEST = test_theme

.PHONY: all clean test install

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^
	@echo "Built $(LIB)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build test program
$(TEST): $(LIB) test_theme.c
	$(CC) $(CFLAGS) test_theme.c -L. -ltheme $(LDFLAGS) -o $(TEST)
	@echo "Built $(TEST)"

test: $(TEST)
	./$(TEST)

install: $(LIB)
	mkdir -p /usr/lib
	mkdir -p /usr/include/theme
	cp $(LIB) /usr/lib/
	cp *.h /usr/include/theme/
	@echo "Installed theme library"

clean:
	rm -f $(OBJS) $(LIB) $(TEST)
	@echo "Cleaned theme engine"

# Dependencies
theme.o: theme.c theme.h parser.h ../../shell/theme/theme_colors.h ../../shell/theme/design_system.h
parser.o: parser.c parser.h theme.h ../../shell/theme/theme_colors.h
