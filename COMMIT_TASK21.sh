#!/bin/bash
#
# Task 21 Commit Script
#
# This script commits all Task 21 implementation files.

set -e

echo "=========================================="
echo "Task 21: Build Scripts & QEMU Testing"
echo "Committing implementation..."
echo "=========================================="
echo ""

# Make scripts executable
chmod +x scripts/build-iso.py
chmod +x scripts/run-qemu.sh
chmod +x scripts/test-boot.sh
chmod +x tests/integration/test_boot.py

echo "✓ Made scripts executable"
echo ""

# Stage new files
git add scripts/build-iso.py
git add scripts/run-qemu.sh
git add scripts/test-boot.sh
git add tests/integration/test_boot.py
git add .gdbinit
git add docs/INTEGRATION_TESTING.md
git add docs/INTEGRATION_REPORT_TEMPLATE.md
git add docs/TASK21_IMPLEMENTATION_SUMMARY.md
git add scripts/README.md
git add QUICKSTART.md

# Stage modified files
git add Makefile
git add README.md

echo "✓ Staged files"
echo ""

# Show what will be committed
echo "Files to be committed:"
git status --short

echo ""
echo "Committing..."

# Commit with detailed message
git commit -m "$(cat <<'EOF'
feat(scripts): add ISO generation and QEMU testing infrastructure

Implement Task 21: Build Scripts & QEMU Testing for Phase 1 integration.

Scripts:
- Add build-iso.py for bootable ISO generation
- Add run-qemu.sh QEMU launcher with debug support
- Add test_boot.py integration test framework
- Add test-boot.sh full build and test orchestration

Testing:
- Automated QEMU boot testing
- Serial log capture and validation
- Critical subsystem initialization checks
- Verbose mode for debugging
- Configurable timeout

Debugging:
- Add .gdbinit with auto-configuration
- Breakpoints on kernel_main and kernel_panic
- Custom GDB commands (pmm-status)

Documentation:
- Add comprehensive integration testing guide
- Add integration report template
- Add quick start guide (5 minutes to boot)
- Add build scripts documentation
- Update main README with full feature set

Build System:
- Add 'make test' target for integration tests
- Add 'make test-full' for full build + test
- Add 'make help' for target documentation

Ready for integration testing once Tasks 2-20 are complete.

Co-Authored-By: Claude Sonnet 4.5 (1M context) <noreply@anthropic.com>
EOF
)"

echo ""
echo "=========================================="
echo "✓ Commit complete!"
echo "=========================================="
echo ""
git log -1 --stat
