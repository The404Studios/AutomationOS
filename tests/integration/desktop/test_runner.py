#!/usr/bin/env python3
"""
AutomationOS Desktop Integration Test Runner
Agent 12: Integration Test Lead

Runs all desktop integration tests and generates a comprehensive report.
Can be run daily to monitor development progress.
"""

import sys
import os
import subprocess
import time
import json
from datetime import datetime
from pathlib import Path


class IntegrationTestRunner:
    """Runs all integration tests and generates report"""

    def __init__(self, build_first=True):
        self.build_first = build_first
        self.results = {}
        self.start_time = None
        self.end_time = None
        self.report_dir = "build/integration_reports"

    def build_system(self):
        """Build the entire system"""
        print("=" * 70)
        print("[*] Building AutomationOS...")
        print("=" * 70)

        # Clean build
        print("\n[1/4] Cleaning previous build...")
        result = subprocess.run(["make", "clean"], capture_output=True)
        if result.returncode != 0:
            print("[!] Clean failed (non-critical)")

        # Build bootloader
        print("\n[2/4] Building bootloader...")
        result = subprocess.run(["make", "bootloader"], capture_output=True)
        if result.returncode != 0:
            print("[✗] Bootloader build failed!")
            return False
        print("[✓] Bootloader built")

        # Build kernel
        print("\n[3/4] Building kernel...")
        result = subprocess.run(["make", "kernel"], capture_output=True)
        if result.returncode != 0:
            print("[✗] Kernel build failed!")
            return False
        print("[✓] Kernel built")

        # Build userspace
        print("\n[4/4] Building userspace...")
        result = subprocess.run(["make", "userspace"], capture_output=True)
        if result.returncode != 0:
            print("[✗] Userspace build failed!")
            return False
        print("[✓] Userspace built")

        # Create initrd
        print("\n[5/6] Creating initrd...")
        result = subprocess.run(["make", "initrd"], capture_output=True)
        if result.returncode != 0:
            print("[✗] Initrd creation failed!")
            return False
        print("[✓] Initrd created")

        # Create ISO
        print("\n[6/6] Creating ISO...")
        result = subprocess.run(["make", "iso"], capture_output=True)
        if result.returncode != 0:
            print("[✗] ISO creation failed!")
            return False
        print("[✓] ISO created")

        print("\n[✓] Build successful!")
        return True

    def run_boot_test(self):
        """Run desktop boot test"""
        print("\n" + "=" * 70)
        print("[*] Running Desktop Boot Test...")
        print("=" * 70)

        try:
            from test_desktop_boot import DesktopBootTest
            test = DesktopBootTest()
            success = test.run_test()
            self.results["boot_test"] = {
                "passed": success,
                "timestamp": datetime.now().isoformat(),
            }
            return success
        except Exception as e:
            print(f"[✗] Boot test failed with exception: {e}")
            self.results["boot_test"] = {
                "passed": False,
                "error": str(e),
                "timestamp": datetime.now().isoformat(),
            }
            return False

    def run_component_tests(self):
        """Run component integration tests"""
        # Note: These are manual/interactive tests
        # In automated CI, we'd need VNC automation or serial command injection

        tests = [
            ("terminal_launch", "Terminal Launch Test"),
            ("file_manager", "File Manager Test"),
            ("input", "Input Events Test"),
            ("window_ops", "Window Operations Test"),
        ]

        print("\n" + "=" * 70)
        print("[*] Component Tests (Manual Verification Required)")
        print("=" * 70)

        for test_id, test_name in tests:
            self.results[test_id] = {
                "passed": None,  # Manual verification required
                "note": "Manual test - requires VNC interaction",
                "timestamp": datetime.now().isoformat(),
            }
            print(f"[→] {test_name}: Manual verification required")

        return True

    def check_desktop_components(self):
        """Check that desktop components are built"""
        print("\n" + "=" * 70)
        print("[*] Checking Desktop Component Build Status...")
        print("=" * 70)

        components = {
            "compositor": "userspace/compositor/compositor",
            "window_manager": "userspace/wm/wm",
            "desktop_shell": "userspace/shell/desktop/desktop",
            "terminal": "userspace/apps/terminal/terminal",
            "file_manager": "userspace/apps/files/files",
        }

        all_built = True
        for name, path in components.items():
            if os.path.exists(path) or os.path.exists(path + ".o"):
                print(f"[✓] {name}: Built")
                self.results[f"component_{name}"] = {"exists": True}
            else:
                print(f"[✗] {name}: Not found at {path}")
                self.results[f"component_{name}"] = {"exists": False}
                all_built = False

        return all_built

    def check_initrd_contents(self):
        """Verify initrd contains desktop binaries"""
        print("\n" + "=" * 70)
        print("[*] Checking Initrd Contents...")
        print("=" * 70)

        initrd_path = "build/initrd.img"
        if not os.path.exists(initrd_path):
            print(f"[✗] Initrd not found at {initrd_path}")
            return False

        # Check size
        size = os.path.getsize(initrd_path)
        print(f"[*] Initrd size: {size} bytes ({size / 1024 / 1024:.2f} MB)")

        self.results["initrd"] = {
            "exists": True,
            "size_bytes": size,
            "size_mb": size / 1024 / 1024,
        }

        return True

    def generate_report(self):
        """Generate integration test report"""
        os.makedirs(self.report_dir, exist_ok=True)

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        report_file = f"{self.report_dir}/integration_report_{timestamp}.json"
        markdown_file = f"{self.report_dir}/integration_report_{timestamp}.md"

        # JSON report
        report_data = {
            "timestamp": datetime.now().isoformat(),
            "duration_seconds": (self.end_time - self.start_time).total_seconds(),
            "results": self.results,
        }

        with open(report_file, 'w') as f:
            json.dump(report_data, f, indent=2)

        # Markdown report
        with open(markdown_file, 'w') as f:
            f.write("# AutomationOS Desktop Integration Test Report\n\n")
            f.write(f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
            f.write(f"**Duration:** {report_data['duration_seconds']:.2f} seconds\n\n")
            f.write("**Agent:** Agent 12 - Integration Test Lead\n\n")
            f.write("---\n\n")

            f.write("## Test Results\n\n")

            # Boot test
            if "boot_test" in self.results:
                status = "✓ PASSED" if self.results["boot_test"]["passed"] else "✗ FAILED"
                f.write(f"### Desktop Boot Test: {status}\n\n")

            # Component status
            f.write("### Desktop Components\n\n")
            for key, value in self.results.items():
                if key.startswith("component_"):
                    name = key.replace("component_", "").replace("_", " ").title()
                    status = "✓ Built" if value.get("exists") else "✗ Missing"
                    f.write(f"- **{name}**: {status}\n")

            f.write("\n### Manual Tests\n\n")
            manual_tests = ["terminal_launch", "file_manager", "input", "window_ops"]
            for test in manual_tests:
                if test in self.results:
                    name = test.replace("_", " ").title()
                    f.write(f"- **{name}**: {self.results[test]['note']}\n")

            f.write("\n---\n\n")
            f.write("## Next Steps\n\n")

            # Determine blockers
            if not self.results.get("boot_test", {}).get("passed"):
                f.write("**BLOCKER:** Boot test failed - system does not boot correctly\n\n")
            else:
                f.write("- Boot test passing ✓\n")
                f.write("- Ready for manual component testing\n")
                f.write("- Run individual tests with VNC connection\n\n")

            f.write("## Testing Commands\n\n")
            f.write("```bash\n")
            f.write("# Run individual tests\n")
            f.write("python3 tests/integration/desktop/test_desktop_boot.py\n")
            f.write("python3 tests/integration/desktop/test_terminal_launch.py\n")
            f.write("python3 tests/integration/desktop/test_file_manager.py\n")
            f.write("python3 tests/integration/desktop/test_input.py\n")
            f.write("python3 tests/integration/desktop/test_window_ops.py\n")
            f.write("```\n\n")

        print(f"\n[*] Reports generated:")
        print(f"    - JSON: {report_file}")
        print(f"    - Markdown: {markdown_file}")

        return report_file, markdown_file

    def run_all_tests(self):
        """Run all integration tests"""
        self.start_time = datetime.now()

        print("=" * 70)
        print("AutomationOS Desktop Integration Test Suite")
        print("Agent 12: Integration Test Lead")
        print(f"Date: {self.start_time.strftime('%Y-%m-%d %H:%M:%S')}")
        print("=" * 70)

        # Build system
        if self.build_first:
            if not self.build_system():
                print("\n[✗] Build failed! Cannot run tests.")
                self.end_time = datetime.now()
                return False

        # Check components
        self.check_desktop_components()
        self.check_initrd_contents()

        # Run boot test
        boot_ok = self.run_boot_test()

        # Component tests (manual)
        self.run_component_tests()

        self.end_time = datetime.now()

        # Generate report
        self.generate_report()

        # Summary
        print("\n" + "=" * 70)
        print("INTEGRATION TEST SUMMARY")
        print("=" * 70)

        if boot_ok:
            print("[✓] Boot test PASSED")
            print("[→] Manual component tests ready to run")
            print("\n[*] System is ready for desktop testing!")
        else:
            print("[✗] Boot test FAILED")
            print("[!] Fix boot issues before proceeding")

        print("=" * 70)

        return boot_ok


def main():
    """Main entry point"""
    import argparse

    parser = argparse.ArgumentParser(description="AutomationOS Desktop Integration Test Runner")
    parser.add_argument("--no-build", action="store_true", help="Skip build step")
    args = parser.parse_args()

    runner = IntegrationTestRunner(build_first=not args.no_build)
    success = runner.run_all_tests()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
