#!/usr/bin/env python3
"""
Phase 2 Integration Test Framework

Provides common utilities for integration testing of security mechanisms.
"""

import os
import sys
import time
import subprocess
import tempfile
import json
from pathlib import Path
from typing import List, Dict, Optional, Tuple
from dataclasses import dataclass


@dataclass
class TestResult:
    """Test result data"""
    name: str
    passed: bool
    duration: float
    message: str = ""
    details: Dict = None


class Colors:
    """ANSI color codes"""
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    MAGENTA = '\033[0;35m'
    CYAN = '\033[0;36m'
    NC = '\033[0m'  # No Color


class QEMURunner:
    """QEMU test environment runner"""

    def __init__(self, iso_path: Path, timeout: int = 30, verbose: bool = False):
        self.iso_path = iso_path
        self.timeout = timeout
        self.verbose = verbose
        self.serial_log = None
        self.proc = None

    def __enter__(self):
        """Start QEMU"""
        self.serial_log = tempfile.NamedTemporaryFile(mode='w+', delete=False, suffix='.log')

        cmd = [
            'qemu-system-x86_64',
            '-cdrom', str(self.iso_path),
            '-m', '4G',
            '-smp', '4',
            '-serial', f'file:{self.serial_log.name}',
            '-display', 'none',
            '-no-reboot',
            '-no-shutdown',
            # Enable KVM if available for better performance
            '-enable-kvm' if sys.platform == 'linux' else '-accel', 'tcg'
        ]

        if self.verbose:
            print(f"{Colors.BLUE}[QEMU] Starting: {' '.join(cmd)}{Colors.NC}")

        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Stop QEMU"""
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

        if self.serial_log:
            self.serial_log.close()

    def wait_for_boot(self, marker: str = "[KERNEL]", timeout: Optional[int] = None) -> bool:
        """Wait for kernel to boot and print marker"""
        timeout = timeout or self.timeout
        start = time.time()

        while time.time() - start < timeout:
            if self.check_serial_contains(marker):
                return True
            time.sleep(0.1)

        return False

    def run_command(self, command: str, expected_output: Optional[str] = None,
                   timeout: int = 5) -> Tuple[bool, str]:
        """Run command in guest and check output"""
        # In a real implementation, this would:
        # 1. Wait for shell prompt
        # 2. Send command via serial
        # 3. Wait for output
        # 4. Verify expected_output if provided

        # For now, this is a placeholder
        # TODO: Implement actual command injection via QEMU monitor
        pass

    def check_serial_contains(self, text: str) -> bool:
        """Check if serial output contains text"""
        if not self.serial_log:
            return False

        try:
            with open(self.serial_log.name, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
                return text in content
        except:
            return False

    def get_serial_output(self) -> str:
        """Get full serial output"""
        if not self.serial_log:
            return ""

        try:
            with open(self.serial_log.name, 'r', encoding='utf-8', errors='ignore') as f:
                return f.read()
        except:
            return ""


class IntegrationTest:
    """Base class for integration tests"""

    def __init__(self, name: str, description: str):
        self.name = name
        self.description = description
        self.results: List[TestResult] = []
        self.verbose = False

    def log(self, message: str, color: str = ''):
        """Print log message"""
        if color:
            print(f"{color}{message}{Colors.NC}")
        else:
            print(message)

    def verbose_log(self, message: str):
        """Print verbose log"""
        if self.verbose:
            print(f"{Colors.BLUE}[VERBOSE] {message}{Colors.NC}")

    def assert_contains(self, output: str, text: str, description: str) -> bool:
        """Assert that output contains text"""
        start = time.time()
        passed = text in output
        duration = time.time() - start

        result = TestResult(
            name=description,
            passed=passed,
            duration=duration,
            message=f"Expected: '{text}'" if not passed else ""
        )
        self.results.append(result)

        if passed:
            self.log(f"  ✓ {description}", Colors.GREEN)
        else:
            self.log(f"  ✗ {description}", Colors.RED)
            self.log(f"    Expected to find: '{text}'", Colors.RED)

        return passed

    def assert_not_contains(self, output: str, text: str, description: str) -> bool:
        """Assert that output does NOT contain text"""
        start = time.time()
        passed = text not in output
        duration = time.time() - start

        result = TestResult(
            name=description,
            passed=passed,
            duration=duration,
            message=f"Should not contain: '{text}'" if not passed else ""
        )
        self.results.append(result)

        if passed:
            self.log(f"  ✓ {description}", Colors.GREEN)
        else:
            self.log(f"  ✗ {description}", Colors.RED)
            self.log(f"    Should not contain: '{text}'", Colors.RED)

        return passed

    def assert_true(self, condition: bool, description: str, message: str = "") -> bool:
        """Assert condition is true"""
        start = time.time()
        passed = condition
        duration = time.time() - start

        result = TestResult(
            name=description,
            passed=passed,
            duration=duration,
            message=message if not passed else ""
        )
        self.results.append(result)

        if passed:
            self.log(f"  ✓ {description}", Colors.GREEN)
        else:
            self.log(f"  ✗ {description}", Colors.RED)
            if message:
                self.log(f"    {message}", Colors.RED)

        return passed

    def print_summary(self) -> bool:
        """Print test summary"""
        passed = sum(1 for r in self.results if r.passed)
        failed = len(self.results) - passed
        total_time = sum(r.duration for r in self.results)

        print("\n" + "=" * 70)
        print(f"Test Suite: {self.name}")
        print("=" * 70)
        print(f"Passed: {passed}/{len(self.results)}")
        print(f"Failed: {failed}/{len(self.results)}")
        print(f"Time:   {total_time:.3f}s")
        print("=" * 70)

        if failed == 0:
            self.log("\n✓ All tests passed!", Colors.GREEN)
            return True
        else:
            self.log(f"\n✗ {failed} test(s) failed", Colors.RED)
            return False

    def run(self) -> bool:
        """Override this method in subclasses"""
        raise NotImplementedError()


class TestSuite:
    """Collection of integration tests"""

    def __init__(self, name: str):
        self.name = name
        self.tests: List[IntegrationTest] = []
        self.verbose = False

    def add_test(self, test: IntegrationTest):
        """Add test to suite"""
        test.verbose = self.verbose
        self.tests.append(test)

    def run_all(self) -> bool:
        """Run all tests in suite"""
        print("=" * 70)
        print(f"Test Suite: {self.name}")
        print("=" * 70)
        print(f"Tests: {len(self.tests)}")
        print("=" * 70)
        print()

        results = []
        for test in self.tests:
            print(f"\n{Colors.CYAN}Running: {test.name}{Colors.NC}")
            print(f"{test.description}")
            print("-" * 70)

            try:
                passed = test.run()
                results.append((test.name, passed))
            except Exception as e:
                print(f"{Colors.RED}ERROR: {e}{Colors.NC}")
                results.append((test.name, False))

        # Print overall summary
        print("\n" + "=" * 70)
        print(f"Overall Results: {self.name}")
        print("=" * 70)

        passed_count = sum(1 for _, p in results if p)
        failed_count = len(results) - passed_count

        for test_name, passed in results:
            status = f"{Colors.GREEN}PASS{Colors.NC}" if passed else f"{Colors.RED}FAIL{Colors.NC}"
            print(f"  {status}  {test_name}")

        print("=" * 70)
        print(f"Total: {passed_count}/{len(results)} passed, {failed_count}/{len(results)} failed")
        print("=" * 70)

        return failed_count == 0


def find_iso(root_dir: Path = None) -> Optional[Path]:
    """Find AutomationOS ISO"""
    if root_dir is None:
        root_dir = Path(__file__).parent.parent.parent.parent

    iso_path = root_dir / 'build' / 'AutomationOS.iso'

    if iso_path.exists():
        return iso_path

    return None


def check_prerequisites() -> Tuple[bool, str]:
    """Check test prerequisites"""
    # Check for QEMU
    try:
        subprocess.run(['qemu-system-x86_64', '--version'],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        return False, "qemu-system-x86_64 not found. Install QEMU."

    # Check for ISO
    iso = find_iso()
    if not iso:
        return False, "AutomationOS.iso not found. Run 'make iso' first."

    return True, ""


def parse_test_output(output: str) -> Dict:
    """Parse test output for structured data"""
    data = {
        'subsystems': [],
        'tests': [],
        'errors': [],
        'warnings': []
    }

    for line in output.split('\n'):
        # Parse subsystem init messages
        if '[' in line and ']' in line and 'initialized' in line.lower():
            subsystem = line.split('[')[1].split(']')[0]
            data['subsystems'].append(subsystem)

        # Parse test messages
        if 'TEST' in line and ('PASS' in line or 'FAIL' in line):
            data['tests'].append(line.strip())

        # Parse errors
        if 'ERROR' in line or 'PANIC' in line:
            data['errors'].append(line.strip())

        # Parse warnings
        if 'WARN' in line or 'WARNING' in line:
            data['warnings'].append(line.strip())

    return data
