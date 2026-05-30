#!/usr/bin/env python3
"""
AutomationOS Phase 2 Integration Test Runner (Python)

Cross-platform test runner for all Phase 2 security integration tests.
Works on Windows, Linux, and macOS.

Usage:
    python3 scripts/run_phase2_tests.py              # Run all tests
    python3 scripts/run_phase2_tests.py --verbose    # Verbose output
    python3 scripts/run_phase2_tests.py --scenario 1 # Run specific scenario
"""

import sys
import os
import subprocess
import argparse
import time
from pathlib import Path
from datetime import datetime
from typing import List, Tuple


class Colors:
    """ANSI color codes (work on Windows 10+ and Unix)"""
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    CYAN = '\033[0;36m'
    NC = '\033[0m'

    @classmethod
    def init(cls):
        """Enable ANSI colors on Windows"""
        if sys.platform == 'win32':
            import ctypes
            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)


class TestRunner:
    """Phase 2 test runner"""

    def __init__(self, verbose: bool = False, scenario: int = None):
        self.verbose = verbose
        self.scenario = scenario
        self.root_dir = Path(__file__).parent.parent
        self.test_dir = self.root_dir / 'tests' / 'integration' / 'phase2'
        self.build_dir = self.root_dir / 'build'
        self.report_dir = self.build_dir / 'test-reports'
        self.results = []

    def log(self, message: str, color: str = ''):
        """Print log message"""
        if color:
            print(f"{color}{message}{Colors.NC}")
        else:
            print(message)

    def log_info(self, message: str):
        self.log(f"[INFO] {message}", Colors.BLUE)

    def log_success(self, message: str):
        self.log(f"[SUCCESS] {message}", Colors.GREEN)

    def log_error(self, message: str):
        self.log(f"[ERROR] {message}", Colors.RED)

    def log_warning(self, message: str):
        self.log(f"[WARNING] {message}", Colors.YELLOW)

    def check_prerequisites(self) -> bool:
        """Check that all prerequisites are met"""
        self.log_info("Checking prerequisites...")

        # Check Python version
        if sys.version_info < (3, 8):
            self.log_error("Python 3.8 or later required")
            return False

        # Check QEMU
        try:
            subprocess.run(['qemu-system-x86_64', '--version'],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except FileNotFoundError:
            self.log_error("qemu-system-x86_64 not found. Install QEMU.")
            return False

        # Check ISO
        iso_path = self.build_dir / 'AutomationOS.iso'
        if not iso_path.exists():
            self.log_error(f"AutomationOS.iso not found at {iso_path}")
            self.log_error("Run 'make iso' first to build the ISO")
            return False

        self.log_success("All prerequisites met")
        return True

    def run_test(self, test_file: Path, test_name: str) -> Tuple[bool, str]:
        """Run a single test scenario"""
        self.log_info(f"Running: {test_name}")

        # Ensure report directory exists
        self.report_dir.mkdir(parents=True, exist_ok=True)

        log_file = self.report_dir / f"{test_name}.log"

        # Build command
        cmd = [sys.executable, str(test_file)]
        if self.verbose:
            cmd.append('--verbose')

        # Run test
        start_time = time.time()
        try:
            result = subprocess.run(
                cmd,
                cwd=str(self.root_dir),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=300  # 5 minute timeout
            )

            duration = time.time() - start_time

            # Write log
            with open(log_file, 'w', encoding='utf-8') as f:
                f.write(result.stdout)

            # Check result
            if result.returncode == 0:
                self.log_success(f"{test_name} - PASSED ({duration:.1f}s)")
                return True, str(log_file)
            else:
                self.log_error(f"{test_name} - FAILED ({duration:.1f}s)")
                self.log_error(f"See log: {log_file}")
                return False, str(log_file)

        except subprocess.TimeoutExpired:
            self.log_error(f"{test_name} - TIMEOUT (> 5 minutes)")
            return False, str(log_file)
        except Exception as e:
            self.log_error(f"{test_name} - ERROR: {e}")
            return False, str(log_file)

    def run_all_tests(self) -> bool:
        """Run all test scenarios"""
        self.log_info("Running all test scenarios...")
        print()

        scenarios = [
            ('test_scenario_web_server.py', 'scenario1-web-server',
             'Scenario 1: Web Server Sandbox'),
            ('test_scenario_untrusted_binary.py', 'scenario2-untrusted-binary',
             'Scenario 2: Untrusted Binary Sandbox'),
            ('test_scenario_container.py', 'scenario3-container',
             'Scenario 3: Container-Like Isolation'),
            ('test_full_stack_security.py', 'scenario4-full-stack',
             'Scenario 4: Full Stack Security Integration'),
        ]

        for test_file, test_id, test_desc in scenarios:
            self.log(f"\n=== {test_desc} ===", Colors.CYAN)
            test_path = self.test_dir / test_file
            passed, log_file = self.run_test(test_path, test_id)
            self.results.append((test_desc, passed, log_file))
            print()

        return all(passed for _, passed, _ in self.results)

    def run_scenario(self, scenario_num: int) -> bool:
        """Run a specific scenario"""
        scenarios = {
            1: ('test_scenario_web_server.py', 'scenario1-web-server',
                'Web Server Sandbox'),
            2: ('test_scenario_untrusted_binary.py', 'scenario2-untrusted-binary',
                'Untrusted Binary Sandbox'),
            3: ('test_scenario_container.py', 'scenario3-container',
                'Container-Like Isolation'),
            4: ('test_full_stack_security.py', 'scenario4-full-stack',
                'Full Stack Security Integration'),
        }

        if scenario_num not in scenarios:
            self.log_error(f"Invalid scenario: {scenario_num}")
            self.log_info("Valid scenarios: 1, 2, 3, 4")
            return False

        test_file, test_id, test_desc = scenarios[scenario_num]
        self.log(f"\n=== Scenario {scenario_num}: {test_desc} ===", Colors.CYAN)
        test_path = self.test_dir / test_file
        passed, log_file = self.run_test(test_path, test_id)
        self.results.append((f"Scenario {scenario_num}: {test_desc}", passed, log_file))

        return passed

    def print_summary(self):
        """Print test results summary"""
        print()
        print("=" * 70)
        print("Test Results Summary")
        print("=" * 70)

        passed = sum(1 for _, p, _ in self.results if p)
        failed = len(self.results) - passed

        print(f"Total:  {len(self.results)}")
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        print("=" * 70)
        print()

        # Print individual results
        for test_name, passed, log_file in self.results:
            status = f"{Colors.GREEN}PASS{Colors.NC}" if passed else f"{Colors.RED}FAIL{Colors.NC}"
            print(f"  {status}  {test_name}")

        print()

        if failed == 0:
            self.log_success("All tests passed!")
        else:
            self.log_error(f"{failed} test(s) failed")
            self.log_info(f"Check logs in: {self.report_dir}")

    def generate_report(self):
        """Generate markdown test report"""
        self.log_info("Generating test report...")

        report_file = self.report_dir / 'phase2-test-report.md'

        passed = sum(1 for _, p, _ in self.results if p)
        failed = len(self.results) - passed
        total = len(self.results)

        with open(report_file, 'w', encoding='utf-8') as f:
            f.write(f"# AutomationOS Phase 2 Integration Test Report\n\n")
            f.write(f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"**Version:** Phase 2 Pre-Release\n")
            f.write(f"**Total Tests:** {total}\n")
            f.write(f"**Passed:** {passed}\n")
            f.write(f"**Failed:** {failed}\n\n")

            f.write("## Test Summary\n\n")
            f.write("| Test | Status | Log File |\n")
            f.write("|------|--------|----------|\n")

            for test_name, test_passed, log_file in self.results:
                status = "✓ PASS" if test_passed else "✗ FAIL"
                log_name = Path(log_file).name if log_file else "N/A"
                f.write(f"| {test_name} | {status} | {log_name} |\n")

            f.write("\n## Test Coverage\n\n")
            f.write("### Security Mechanisms Tested\n\n")
            f.write("- ✓ Capability-based security\n")
            f.write("- ✓ Namespace isolation (PID, Mount, Network, IPC, UTS)\n")
            f.write("- ✓ Mandatory Access Control (MAC)\n")
            f.write("- ✓ Resource limits (rlimit)\n")
            f.write("- ✓ Audit logging\n")
            f.write("- ✓ Syscall filtering\n")
            f.write("- ✓ Security boundary enforcement\n\n")

            f.write("### Test Scenarios\n\n")
            f.write("1. **Web Server Sandbox**: Realistic web server with MAC, capabilities, namespaces\n")
            f.write("2. **Untrusted Binary Sandbox**: Maximum security isolation\n")
            f.write("3. **Container-Like Isolation**: Docker-like container isolation\n")
            f.write("4. **Full Stack Security**: Comprehensive integration tests\n\n")

            f.write("## Next Steps\n\n")
            if failed == 0:
                f.write("- All tests passed! Phase 2 security implementation is ready.\n")
                f.write("- Proceed to performance profiling\n")
                f.write("- Begin Phase 3 (Networking & AI) development\n")
            else:
                f.write("- Fix failed tests\n")
                f.write("- Re-run test suite\n")
                f.write("- Review test logs for failure details\n")

            f.write("\n---\n\n")
            f.write("Generated by AutomationOS Test Suite\n")

        self.log_success(f"Test report generated: {report_file}")

    def run(self) -> int:
        """Main test runner"""
        print("=" * 70)
        print("AutomationOS Phase 2 Integration Test Suite")
        print("=" * 70)
        print()

        # Check prerequisites
        if not self.check_prerequisites():
            return 2

        print()

        # Run tests
        if self.scenario:
            success = self.run_scenario(self.scenario)
        else:
            success = self.run_all_tests()

        # Print summary
        self.print_summary()

        # Generate report
        self.generate_report()

        return 0 if success else 1


def main():
    """Main entry point"""
    # Enable colors on Windows
    Colors.init()

    # Parse arguments
    parser = argparse.ArgumentParser(
        description='AutomationOS Phase 2 Integration Test Runner'
    )
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Verbose output')
    parser.add_argument('-s', '--scenario', type=int, metavar='N',
                       help='Run specific scenario (1-4)')

    args = parser.parse_args()

    # Run tests
    runner = TestRunner(verbose=args.verbose, scenario=args.scenario)
    return runner.run()


if __name__ == '__main__':
    sys.exit(main())
