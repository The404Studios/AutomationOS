# AutomationOS Driver Test Suite Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O0 -I.
LDFLAGS = -lpthread -lm

# Source files
FRAMEWORK_SRC = driver_test_framework.c
TEST_SRC = driver_test_main.c test_serial.c test_ps2.c test_nvme.c test_ahci.c test_network.c test_graphics.c test_audio.c test_usb.c test_driver_integration.c
# Optional: test_pit.c test_framebuffer.c (not yet implemented)

# Object files
FRAMEWORK_OBJ = $(FRAMEWORK_SRC:.c=.o)
TEST_OBJ = $(TEST_SRC:.c=.o)

# Output binary
TARGET = driver_test_suite

# Default target
all: $(TARGET)

# Build test suite
$(TARGET): $(FRAMEWORK_OBJ) $(TEST_OBJ)
	@echo "[LD] Linking $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[OK] Build complete: $(TARGET)"

# Compile object files
%.o: %.c
	@echo "[CC] Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Run all tests
test: $(TARGET)
	@echo ""
	@echo "Running all driver tests..."
	@./$(TARGET) --all

# Run quick tests
quick: $(TARGET)
	@echo ""
	@echo "Running quick tests..."
	@./$(TARGET) --all --quick

# Run specific test suites
test-serial: $(TARGET)
	@./$(TARGET) --serial

test-ps2: $(TARGET)
	@./$(TARGET) --ps2

test-pit: $(TARGET)
	@./$(TARGET) --pit

test-framebuffer: $(TARGET)
	@./$(TARGET) --framebuffer

test-storage: $(TARGET)
	@./$(TARGET) --storage

test-network: $(TARGET)
	@./$(TARGET) --network

test-graphics: $(TARGET)
	@./$(TARGET) --graphics

test-audio: $(TARGET)
	@./$(TARGET) --audio

test-usb: $(TARGET)
	@./$(TARGET) --usb

test-integration: $(TARGET)
	@./$(TARGET) --test driver_integration

# Run stress tests
stress: $(TARGET)
	@./$(TARGET) --stress

# Run with valgrind (memory leak detection)
valgrind: $(TARGET)
	@echo "Running tests with valgrind..."
	@valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) --all

# Generate coverage report (requires gcov)
coverage: CFLAGS += -fprofile-arcs -ftest-coverage
coverage: LDFLAGS += -lgcov --coverage
coverage: clean $(TARGET)
	@./$(TARGET) --all
	@gcov $(TEST_SRC)
	@echo "Coverage report generated: *.gcov"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -f $(TARGET) *.o *.gcov *.gcda *.gcno
	@echo "Clean complete"

# Help
help:
	@echo "AutomationOS Driver Test Suite Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build test suite (default)"
	@echo "  test             - Run all tests"
	@echo "  quick            - Run quick tests (skip stress tests)"
	@echo "  test-serial      - Run serial driver tests"
	@echo "  test-ps2         - Run PS/2 keyboard tests"
	@echo "  test-pit         - Run PIT timer tests"
	@echo "  test-framebuffer - Run framebuffer tests"
	@echo "  test-storage     - Run storage driver tests (NVMe, AHCI)"
	@echo "  test-network     - Run network driver tests (e1000)"
	@echo "  test-graphics    - Run graphics driver tests (i915)"
	@echo "  test-audio       - Run audio driver tests (HDA)"
	@echo "  test-usb         - Run USB driver tests (xHCI, HID)"
	@echo "  stress           - Run stress tests"
	@echo "  valgrind         - Run tests with valgrind"
	@echo "  coverage         - Generate code coverage report"
	@echo "  clean            - Remove build artifacts"
	@echo "  help             - Show this help message"

.PHONY: all test quick test-serial test-ps2 test-pit test-framebuffer test-storage test-network test-graphics test-audio test-usb stress valgrind coverage clean help
