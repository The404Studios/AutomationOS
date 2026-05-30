# Makefile for AutoShell
#
# Build the AutoShell shell and associated utilities

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lreadline -lncurses
INSTALL_DIR = /usr/local/bin
SCRIPTS_DIR = /usr/share/autoshell/scripts

# Source files
AUTOSHELL_SRCS = autoshell.c autoshell_builtins.c autoshell_script.c completion.c
AUTOSHELL_OBJS = $(AUTOSHELL_SRCS:.c=.o)

# Targets
all: autoshell

autoshell: $(AUTOSHELL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(AUTOSHELL_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(AUTOSHELL_OBJS) autoshell

install: autoshell
	install -d $(INSTALL_DIR)
	install -m 755 autoshell $(INSTALL_DIR)/
	install -d $(SCRIPTS_DIR)
	install -m 755 ../../usr/share/scripts/*.sh $(SCRIPTS_DIR)/
	@echo "AutoShell installed successfully"
	@echo "Add to /etc/shells: echo $(INSTALL_DIR)/autoshell | sudo tee -a /etc/shells"
	@echo "Change shell: chsh -s $(INSTALL_DIR)/autoshell"

uninstall:
	rm -f $(INSTALL_DIR)/autoshell
	rm -rf $(SCRIPTS_DIR)

test:
	@echo "Running AutoShell tests..."
	@./autoshell -c "echo 'Test 1: Basic echo'"
	@./autoshell -c "pwd"
	@./autoshell -c "export TEST=value && echo \$TEST"
	@echo "Tests completed"

.PHONY: all clean install uninstall test
