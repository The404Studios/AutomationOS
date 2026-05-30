#!/usr/bin/env python3
"""
AutomationOS Boot Integration Test

Tests that AutomationOS boots successfully in QEMU and all subsystems initialize.

Usage:
    python3 tests/integration/test_boot.py
    python3 tests/integration/test_boot.py --verbose
    python3 tests/integration/test_boot.py --timeout 30

Exit codes:
    0 - All tests passed
    1 - Test failed
    2 - Setup error (QEMU not found, ISO missing, etc.)
"""

import os
import sys
import time
import shutil
import subprocess
import argparse
from pathlib import Path


class Colors:
    """ANSI color codes"""
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    NC = '\033[0m'  # No Color


class BootTest:
    """Boot integration test runner"""

    def __init__(self, timeout=15, verbose=False):
        self.root = Path(__file__).parent.parent.parent
        self.iso_path = self.root / 'build' / 'automationos.iso'
        self.serial_log = self.root / 'build' / 'serial.log'
        self.timeout = timeout
        self.verbose = verbose
        self.tests_passed = 0
        self.tests_failed = 0

    def log(self, message, color=''):
        """Print log message"""
        if color:
            print(f"{color}{message}{Colors.NC}")
        else:
            print(message)

    def verbose_log(self, message):
        """Print verbose log message"""
        if self.verbose:
            print(f"{Colors.BLUE}[VERBOSE] {message}{Colors.NC}")

    def check_prerequisites(self):
        """Check that required files and tools exist"""
        self.log("Checking prerequisites...", Colors.YELLOW)

        # Check for QEMU
        qemu_binary = shutil.which('qemu-system-x86_64')
        if not qemu_binary and sys.platform == 'win32':
            # Fallback: check common Windows install paths
            win_paths = [
                r'C:\Program Files\qemu\qemu-system-x86_64.exe',
                r'C:\Program Files (x86)\qemu\qemu-system-x86_64.exe',
            ]
            for p in win_paths:
                if Path(p).exists():
                    qemu_binary = p
                    break
        if not qemu_binary:
            self.log("ERROR: qemu-system-x86_64 not found", Colors.RED)
            self.log("Install QEMU to run tests")
            return False
        self.verbose_log(f"QEMU found: {qemu_binary}")

        # Check for ISO
        if not self.iso_path.exists():
            self.log(f"ERROR: ISO not found: {self.iso_path}", Colors.RED)
            self.log("Run 'make iso' first to build the ISO", Colors.RED)
            return False
        self.verbose_log(f"ISO found: {self.iso_path}")

        self.log("  ✓ All prerequisites met", Colors.GREEN)
        return True

    def run_qemu(self):
        """Run QEMU and capture serial output"""
        self.log(f"Starting QEMU (timeout: {self.timeout}s)...", Colors.YELLOW)

        # Remove old serial log
        if self.serial_log.exists():
            self.serial_log.unlink()

        cmd = [
            'qemu-system-x86_64',
            '-cdrom', str(self.iso_path),
            '-m', '4G',
            '-smp', '4',
            '-serial', f'file:{self.serial_log}',
            '-display', 'none',
            '-no-reboot',
            '-no-shutdown'
        ]

        self.verbose_log(f"QEMU command: {' '.join(cmd)}")

        try:
            kwargs = {
                'stdout': subprocess.PIPE,
                'stderr': subprocess.PIPE,
            }
            if sys.platform == 'win32':
                kwargs['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
            proc = subprocess.Popen(cmd, **kwargs)

            self.verbose_log(f"QEMU PID: {proc.pid}")

            # Wait for timeout
            time.sleep(self.timeout)

            # Terminate QEMU
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.verbose_log("Force killing QEMU")
                proc.kill()
                proc.wait()

            self.log("  ✓ QEMU run complete", Colors.GREEN)
            return True

        except Exception as e:
            self.log(f"ERROR: Failed to run QEMU: {e}", Colors.RED)
            return False

    def read_serial_output(self):
        """Read serial output from log file"""
        self.verbose_log(f"Reading serial log: {self.serial_log}")

        if not self.serial_log.exists():
            self.log(f"ERROR: Serial log not found: {self.serial_log}", Colors.RED)
            return None

        with open(self.serial_log, 'r', encoding='utf-8', errors='ignore') as f:
            output = f.read()

        self.verbose_log(f"Serial output length: {len(output)} bytes")
        return output

    def assert_contains(self, output, text, description):
        """Assert that output contains specific text"""
        if text in output:
            self.log(f"  ✓ {description}", Colors.GREEN)
            self.tests_passed += 1
            return True
        else:
            self.log(f"  ✗ {description}", Colors.RED)
            self.log(f"    Expected: '{text}'", Colors.RED)
            self.tests_failed += 1
            return False

    def run_tests(self, output):
        """Run all boot tests"""
        self.log("\nRunning boot tests...", Colors.YELLOW)

        # Critical boot tests
        self.assert_contains(output, 'AutomationOS', 'Kernel banner printed')
        self.assert_contains(output, '[PMM]', 'Physical Memory Manager initialized')
        self.assert_contains(output, '[VMM]', 'Virtual Memory Manager initialized')
        self.assert_contains(output, '[HEAP]', 'Kernel heap initialized')
        self.assert_contains(output, '[GDT]', 'Global Descriptor Table loaded')
        self.assert_contains(output, '[IDT]', 'Interrupt Descriptor Table loaded')
        self.assert_contains(output, '[PIT]', 'Programmable Interval Timer initialized')
        self.assert_contains(output, '[KERNEL]', 'Kernel main initialized')

        # Optional subsystem tests (may not be implemented yet)
        # These won't fail the test suite, just report status
        optional_tests = [
            ('[PS2]', 'PS/2 keyboard initialized'),
            ('[FB]', 'Framebuffer initialized'),
            ('[SCHED]', 'Scheduler initialized'),
            ('[INIT]', 'Init process started'),
            ('[SHELL]', 'Shell started'),
        ]

        self.log("\nOptional subsystem tests:", Colors.YELLOW)
        for text, description in optional_tests:
            if text in output:
                self.log(f"  ✓ {description}", Colors.GREEN)
            else:
                self.log(f"  - {description} (not yet implemented)", Colors.YELLOW)

    def print_summary(self):
        """Print test summary"""
        print("\n" + "=" * 50)
        print("Test Summary")
        print("=" * 50)
        print(f"Passed: {self.tests_passed}")
        print(f"Failed: {self.tests_failed}")
        print(f"Total:  {self.tests_passed + self.tests_failed}")
        print("=" * 50)

        if self.tests_failed == 0:
            self.log("\n✓ All tests passed!", Colors.GREEN)
            return True
        else:
            self.log(f"\n✗ {self.tests_failed} test(s) failed", Colors.RED)
            return False

    def print_serial_output(self, output):
        """Print serial output for debugging"""
        print("\n" + "=" * 50)
        print("Serial Console Output")
        print("=" * 50)
        print(output)
        print("=" * 50)

    def run(self):
        """Main test runner"""
        print("=" * 50)
        print("AutomationOS Boot Integration Test")
        print("=" * 50)
        print()

        # Check prerequisites
        if not self.check_prerequisites():
            return 2

        # Run QEMU
        if not self.run_qemu():
            return 2

        # Read serial output
        output = self.read_serial_output()
        if output is None:
            return 2

        # Print output if verbose
        if self.verbose:
            self.print_serial_output(output)

        # Run tests
        self.run_tests(output)

        # Print summary
        if self.print_summary():
            return 0
        else:
            # Print serial output for debugging failed tests
            if not self.verbose:
                self.print_serial_output(output)
            return 1


def main():
    parser = argparse.ArgumentParser(
        description='AutomationOS Boot Integration Test'
    )
    parser.add_argument(
        '--timeout',
        type=int,
        default=15,
        help='QEMU timeout in seconds (default: 15)'
    )
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Verbose output'
    )

    args = parser.parse_args()

    test = BootTest(timeout=args.timeout, verbose=args.verbose)
    return test.run()


if __name__ == '__main__':
    sys.exit(main())
