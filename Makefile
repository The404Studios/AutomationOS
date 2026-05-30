# AutomationOS Build System
.PHONY: all clean bootloader kernel userspace initrd iso qemu test help
.PHONY: analyze analyze-all analyze-report cppcheck sparse clang-tidy
.PHONY: analyze-incremental analyze-weekly analyze-kernel-core analyze-drivers
.PHONY: format check-format lint install-hooks

# Toolchain
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AS = nasm
PYTHON = python3
BASH = bash

# Code Quality Tools
CLANG_FORMAT = clang-format
CLANG_TIDY = clang-tidy
CLANG_ANALYZER = scan-build
CPPCHECK = cppcheck
SPARSE = sparse

# Directories
BUILD_DIR = build
ISO_DIR = iso
ANALYSIS_DIR = $(BUILD_DIR)/static-analysis
COVERAGE_DIR = docs/coverage

# Targets
all: bootloader kernel userspace initrd iso

validate: all
	$(BASH) scripts/validate-build.sh

bootloader:
	$(MAKE) -C boot/

kernel:
	$(MAKE) -C kernel/

userspace:
	$(MAKE) -C userspace/

initrd: userspace
	$(BASH) scripts/mkinitrd.sh

iso: bootloader kernel userspace initrd
	$(PYTHON) scripts/build-iso.py

qemu: iso
	$(BASH) scripts/run-qemu.sh

qemu-debug: iso
	$(BASH) scripts/run-qemu.sh --debug

test: iso
	$(BASH) scripts/test-boot.sh --skip-build

test-full:
	$(BASH) scripts/test-boot.sh

# Unit tests with coverage
unit-tests:
	$(MAKE) -C tests/unit all
	$(MAKE) -C tests/unit run

# Coverage targets
coverage-build:
	$(MAKE) -C tests/unit COVERAGE=1 clean all

coverage-test: coverage-build
	$(MAKE) -C tests/unit run

coverage-report: coverage-test
	@echo "Generating coverage report..."
	@mkdir -p $(COVERAGE_DIR)
	lcov --capture --directory tests/unit --directory kernel --output-file coverage.info 2>/dev/null || echo "Warning: lcov capture had issues"
	lcov --remove coverage.info '/usr/*' --output-file coverage_filtered.info 2>/dev/null || echo "Warning: lcov remove had issues"
	genhtml coverage_filtered.info --output-directory $(COVERAGE_DIR) 2>/dev/null || echo "Warning: genhtml had issues"
	@echo "Coverage report generated in $(COVERAGE_DIR)/index.html"

coverage: coverage-report
	@echo ""
	@echo "==========================================="
	@echo "  Coverage Report Summary"
	@echo "==========================================="
	@lcov --list coverage_filtered.info 2>/dev/null || echo "No coverage data available"
	@echo ""
	@echo "Open $(COVERAGE_DIR)/index.html to view detailed report"

coverage-clean:
	find . -name "*.gcda" -delete 2>/dev/null || true
	find . -name "*.gcno" -delete 2>/dev/null || true
	rm -f coverage.info coverage_filtered.info
	rm -rf $(COVERAGE_DIR)

clean:
	$(MAKE) -C boot/ clean
	$(MAKE) -C kernel/ clean
	$(MAKE) -C userspace/ clean
	$(MAKE) -C tests/unit clean 2>/dev/null || true
	rm -rf $(BUILD_DIR) $(ISO_DIR)
	$(MAKE) coverage-clean 2>/dev/null || true

# ============================================================================
# Static Analysis Targets
# ============================================================================

# Run all static analysis tools
analyze-all:
	@echo "=== Running Complete Static Analysis Suite ==="
	@mkdir -p $(ANALYSIS_DIR)
	@echo "Starting analysis at $$(date)" > $(ANALYSIS_DIR)/latest-scan.txt
	@$(MAKE) analyze 2>&1 | tee -a $(ANALYSIS_DIR)/latest-scan.txt
	@$(MAKE) cppcheck 2>&1 | tee -a $(ANALYSIS_DIR)/latest-scan.txt
	@$(MAKE) sparse 2>&1 | tee -a $(ANALYSIS_DIR)/latest-scan.txt
	@$(MAKE) clang-tidy 2>&1 | tee -a $(ANALYSIS_DIR)/latest-scan.txt
	@echo "Analysis complete at $$(date)" >> $(ANALYSIS_DIR)/latest-scan.txt
	@$(PYTHON) scripts/static-analysis/generate-report.py
	@echo ""
	@echo "=== Analysis Summary ==="
	@cat $(ANALYSIS_DIR)/summary.txt
	@echo ""
	@echo "Full results: $(ANALYSIS_DIR)/latest-scan.txt"

# Clang Static Analyzer (deep dataflow analysis)
analyze:
	@echo "=== Running Clang Static Analyzer ==="
	@mkdir -p $(ANALYSIS_DIR)/scan-results
	@$(CLANG_ANALYZER) --use-cc=$(CC) --status-bugs \
		-o $(ANALYSIS_DIR)/scan-results \
		--exclude kernel/lib/ \
		$(MAKE) kernel 2>&1 | tee $(ANALYSIS_DIR)/clang-analyzer.log
	@echo "Results: $(ANALYSIS_DIR)/scan-results/"

# Generate HTML report from Clang Static Analyzer
analyze-report: analyze
	@echo "=== Generating HTML Report ==="
	@if [ -d $(ANALYSIS_DIR)/scan-results ]; then \
		echo "Report available at: $(ANALYSIS_DIR)/scan-results/index.html"; \
		echo "Open with: xdg-open $(ANALYSIS_DIR)/scan-results/*/index.html"; \
	else \
		echo "No issues found - no report generated"; \
	fi

# Cppcheck (common C/C++ bugs)
cppcheck:
	@echo "=== Running Cppcheck ==="
	@mkdir -p $(ANALYSIS_DIR)
	@$(CPPCHECK) --enable=all \
		--suppress=missingInclude \
		--suppress=unusedFunction \
		--inline-suppr \
		--quiet \
		--template='{file}:{line}:{severity}:{message}' \
		-I kernel/include \
		-I userspace/libc/include \
		--platform=unix64 \
		--std=c11 \
		kernel/ userspace/ 2>&1 | tee $(ANALYSIS_DIR)/cppcheck.log
	@echo "Results: $(ANALYSIS_DIR)/cppcheck.log"

# Sparse (Linux kernel semantic checker)
sparse:
	@echo "=== Running Sparse ==="
	@mkdir -p $(ANALYSIS_DIR)
	@echo "Checking kernel sources with Sparse..."
	@find kernel -name "*.c" -exec $(SPARSE) \
		-D__KERNEL__ \
		-Waddress-space \
		-Wcontext \
		-Wno-decl \
		-I kernel/include \
		{} \; 2>&1 | tee $(ANALYSIS_DIR)/sparse.log
	@echo "Results: $(ANALYSIS_DIR)/sparse.log"

# Clang-Tidy (lint with custom checks)
clang-tidy:
	@echo "=== Running Clang-Tidy ==="
	@mkdir -p $(ANALYSIS_DIR)
	@if [ -f .clang-tidy ]; then \
		find kernel -name "*.c" | head -20 | xargs $(CLANG_TIDY) \
			--config-file=.clang-tidy \
			-- -I kernel/include -std=gnu11 2>&1 | tee $(ANALYSIS_DIR)/clang-tidy.log; \
	else \
		echo "Warning: .clang-tidy config not found, using default checks"; \
		find kernel -name "*.c" | head -20 | xargs $(CLANG_TIDY) \
			-checks='bugprone-*,clang-analyzer-*,cert-*,concurrency-*' \
			-- -I kernel/include -std=gnu11 2>&1 | tee $(ANALYSIS_DIR)/clang-tidy.log; \
	fi
	@echo "Results: $(ANALYSIS_DIR)/clang-tidy.log"

# Incremental analysis (only changed files)
analyze-incremental:
	@echo "=== Running Incremental Static Analysis ==="
	@mkdir -p $(ANALYSIS_DIR)
	@if [ -d .git ]; then \
		git diff --name-only HEAD | grep '\.c$$' | while read file; do \
			echo "Analyzing $$file..."; \
			$(CPPCHECK) --enable=all --quiet $$file; \
			$(SPARSE) -D__KERNEL__ -I kernel/include $$file 2>&1; \
		done | tee $(ANALYSIS_DIR)/incremental.log; \
	else \
		echo "Not a git repository - running full analysis"; \
		$(MAKE) analyze-all; \
	fi

# Weekly comprehensive scan (with detailed report)
analyze-weekly:
	@echo "=== Weekly Comprehensive Static Analysis Scan ==="
	@mkdir -p $(ANALYSIS_DIR)/weekly
	@$(PYTHON) scripts/static-analysis/weekly-scan.py
	@echo "Weekly report: $(ANALYSIS_DIR)/weekly/report-$$(date +%Y-%m-%d).txt"

# Analyze specific subsystems
analyze-kernel-core:
	@echo "=== Analyzing Kernel Core ==="
	@$(CPPCHECK) --enable=all kernel/core/ 2>&1 | tee $(ANALYSIS_DIR)/kernel-core.log
	@$(SPARSE) kernel/core/*.c 2>&1 | tee -a $(ANALYSIS_DIR)/kernel-core.log

analyze-drivers:
	@echo "=== Analyzing Drivers ==="
	@$(CPPCHECK) --enable=all kernel/drivers/ 2>&1 | tee $(ANALYSIS_DIR)/drivers.log
	@$(SPARSE) kernel/drivers/*.c 2>&1 | tee -a $(ANALYSIS_DIR)/drivers.log

# ============================================================================
# Code Quality and Formatting Targets
# ============================================================================

# Format all C/H files with clang-format
format:
	@echo "=== Formatting C/H files with clang-format ==="
	@if ! command -v $(CLANG_FORMAT) &> /dev/null; then \
		echo "Error: clang-format not found. Install with: apt install clang-format"; \
		exit 1; \
	fi
	@find kernel userspace boot tests -name '*.c' -o -name '*.h' | while read file; do \
		echo "Formatting $$file..."; \
		$(CLANG_FORMAT) -i -style=file "$$file"; \
	done
	@echo "✓ Formatting complete"

# Check formatting without modifying files
check-format:
	@echo "=== Checking code formatting ==="
	@if ! command -v $(CLANG_FORMAT) &> /dev/null; then \
		echo "Error: clang-format not found. Install with: apt install clang-format"; \
		exit 1; \
	fi
	@FAILED=0; \
	find kernel userspace boot tests -name '*.c' -o -name '*.h' | while read file; do \
		if ! $(CLANG_FORMAT) -style=file --dry-run --Werror "$$file" 2>&1 | grep -q "warning:"; then \
			echo "✓ $$file"; \
		else \
			echo "✗ Format issues in $$file"; \
			FAILED=1; \
		fi; \
	done; \
	if [ $$FAILED -ne 0 ]; then \
		echo ""; \
		echo "Format check failed. Run 'make format' to fix."; \
		exit 1; \
	fi; \
	echo "✓ All files are properly formatted"

# Run linter (clang-tidy) on all C files
lint:
	@echo "=== Running clang-tidy linter ==="
	@if ! command -v $(CLANG_TIDY) &> /dev/null; then \
		echo "Error: clang-tidy not found. Install with: apt install clang-tidy"; \
		exit 1; \
	fi
	@mkdir -p $(ANALYSIS_DIR)
	@FAILED=0; \
	echo "Analyzing C files..."; \
	find kernel userspace boot -name '*.c' | while read file; do \
		echo "  Checking $$file..."; \
		if $(CLANG_TIDY) "$$file" -- -Ikernel/include -std=gnu11 2>&1 | tee -a $(ANALYSIS_DIR)/lint.log | grep -q "warning:"; then \
			FAILED=1; \
		fi; \
	done; \
	if [ $$FAILED -ne 0 ]; then \
		echo ""; \
		echo "✗ Linter found warnings. See $(ANALYSIS_DIR)/lint.log for details"; \
		exit 1; \
	fi; \
	echo "✓ Linter passed with no warnings"

# Install git pre-commit hooks
install-hooks:
	@echo "=== Installing git pre-commit hooks ==="
	@if [ -f scripts/install-hooks.sh ]; then \
		$(BASH) scripts/install-hooks.sh; \
	else \
		cp scripts/pre-commit .git/hooks/pre-commit; \
		chmod +x .git/hooks/pre-commit; \
		echo "✓ Pre-commit hooks installed"; \
	fi

# ============================================================================
# CI/CD Targets
# ============================================================================

# CI build (clean + all)
ci-build: clean all
	@echo "✓ CI build completed successfully"

# CI test suite
ci-test: unit-tests test-integration
	@echo "✓ CI tests completed successfully"

# Run security checks
security-check:
	@echo "=== Running security checks ==="
	@if command -v cppcheck &> /dev/null; then \
		$(MAKE) cppcheck; \
	else \
		echo "Warning: cppcheck not found"; \
	fi
	@if command -v clang-tidy &> /dev/null; then \
		$(MAKE) clang-tidy; \
	else \
		echo "Warning: clang-tidy not found"; \
	fi

# Build Docker image
docker-build:
	@echo "=== Building Docker image ==="
	docker build -f Dockerfile.build -t automationos-build:latest .
	@echo "✓ Docker image built successfully"

# Test in Docker
docker-test:
	@echo "=== Running tests in Docker ==="
	docker run --rm -v $$(pwd):/workspace automationos-build:latest make test
	@echo "✓ Docker tests completed"

# Test integration
test-integration:
	$(PYTHON) tests/integration/test_boot.py --verbose

# Benchmark tests
test-bench:
	@if [ -d tests/bench ]; then \
		$(MAKE) -C tests/bench run; \
	else \
		echo "Benchmark tests not yet implemented"; \
	fi

help:
	@echo "AutomationOS Build System"
	@echo ""
	@echo "Build Targets:"
	@echo "  make all          - Build bootloader, kernel, userspace, and ISO"
	@echo "  make bootloader   - Build AutoBoot UEFI bootloader"
	@echo "  make kernel       - Build kernel"
	@echo "  make userspace    - Build userspace programs"
	@echo "  make iso          - Generate bootable ISO"
	@echo "  make clean        - Clean build artifacts"
	@echo ""
	@echo "Testing Targets:"
	@echo "  make qemu         - Run in QEMU"
	@echo "  make qemu-debug   - Run in QEMU with GDB server"
	@echo "  make test         - Run integration tests (uses existing build)"
	@echo "  make test-full    - Full build and test"
	@echo "  make unit-tests   - Build and run unit tests"
	@echo "  make validate     - Validate build output and check all components"
	@echo ""
	@echo "Code Quality Targets:"
	@echo "  make format       - Auto-format all C/H files (clang-format)"
	@echo "  make check-format - Check formatting without modifying files"
	@echo "  make lint         - Run static analysis linter (clang-tidy)"
	@echo "  make install-hooks - Install git pre-commit hooks"
	@echo ""
	@echo "Coverage Targets:"
	@echo "  make coverage-build   - Build unit tests with coverage instrumentation"
	@echo "  make coverage-test    - Run coverage tests"
	@echo "  make coverage-report  - Generate HTML coverage report"
	@echo "  make coverage         - Full coverage workflow (build + test + report)"
	@echo "  make coverage-clean   - Remove coverage data files"
	@echo ""
	@echo "Static Analysis Targets:"
	@echo "  make analyze-all       - Run all static analysis tools (2-5 min)"
	@echo "  make analyze           - Run Clang Static Analyzer"
	@echo "  make analyze-report    - Generate HTML report from analyzer"
	@echo "  make cppcheck          - Run Cppcheck (C/C++ bug detector)"
	@echo "  make sparse            - Run Sparse (kernel semantic checker)"
	@echo "  make clang-tidy        - Run Clang-Tidy (lint with custom checks)"
	@echo "  make analyze-incremental - Analyze only changed files (git)"
	@echo "  make analyze-weekly    - Weekly comprehensive scan with report"
	@echo "  make analyze-kernel-core - Analyze kernel/core/ subsystem"
	@echo "  make analyze-drivers   - Analyze kernel/drivers/ subsystem"
	@echo ""
	@echo "CI/CD Targets:"
	@echo "  make ci-build        - CI build (clean + all)"
	@echo "  make ci-test         - CI test suite (unit + integration)"
	@echo "  make security-check  - Run security analysis"
	@echo "  make docker-build    - Build Docker container"
	@echo "  make docker-test     - Test in Docker"
	@echo "  make test-integration - Run integration tests"
	@echo "  make test-bench      - Run benchmark tests"
	@echo ""
	@echo "Help:"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Documentation:"
	@echo "  docs/CODING_STANDARDS.md - Code quality guidelines"
	@echo "  docs/STATIC_ANALYSIS.md  - Static analysis documentation"
	@echo "  docs/CI_CD.md            - CI/CD pipeline documentation"
	@echo "  docs/CONTRIBUTING.md     - Contributing guide"
	@echo ""

.PHONY: all clean bootloader kernel userspace iso qemu qemu-debug test test-full validate help
.PHONY: analyze analyze-all analyze-report cppcheck sparse clang-tidy
.PHONY: analyze-incremental analyze-weekly analyze-kernel-core analyze-drivers
.PHONY: unit-tests coverage-build coverage-test coverage-report coverage coverage-clean
.PHONY: ci-build ci-test security-check docker-build docker-test test-integration test-bench
.PHONY: format check-format lint install-hooks
.PHONY: format check-format lint install-hooks
