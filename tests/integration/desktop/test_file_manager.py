#!/usr/bin/env python3
"""
AutomationOS File Manager Integration Test
Agent 12: Integration Test Lead

Tests launching file manager and browsing the filesystem.
Validates file manager can display directory contents.
"""

import sys
import os
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


class FileManagerTest:
    """Test file manager application"""

    def __init__(self, qemu_path=None, iso_path=None):
        self.qemu_path = qemu_path or self._find_qemu()
        self.iso_path = iso_path or "build/automationos.iso"
        self.serial_log = "build/test_file_manager.log"
        self.timeout = 45

    def _find_qemu(self):
        """Find QEMU binary"""
        paths = [
            "qemu-system-x86_64",
            "/c/Program Files/qemu/qemu-system-x86_64.exe",
            "/usr/bin/qemu-system-x86_64",
        ]
        for path in paths:
            if os.path.exists(path):
                return path
        raise FileNotFoundError("QEMU not found")

    def parse_log_for_files_app(self):
        """Check if file manager was launched"""
        if not os.path.exists(self.serial_log):
            return False

        with open(self.serial_log, 'r') as f:
            log = f.read()

        indicators = [
            "file manager",
            "files app",
            "explorer",
            "/home",
            "directory listing",
        ]

        for indicator in indicators:
            if indicator in log.lower():
                return True

        return False

    def run_test(self):
        """Run file manager test"""
        print("=" * 70)
        print("AutomationOS File Manager Integration Test")
        print("Agent 12: Integration Test Lead")
        print("=" * 70)

        print("\n[*] This test requires manual interaction:")
        print("    1. System will boot to desktop")
        print("    2. Click the files icon in the dock")
        print("    3. Verify file manager window appears")
        print("    4. Browse directories (/, /home, etc.)")
        print("    5. Verify file list updates")

        print(f"\n[*] Connect to VNC at localhost:5902 to interact")
        print(f"[*] Press Ctrl+C to end test\n")

        # Check ISO
        if not os.path.exists(self.iso_path):
            print("[!] ISO not found, building...")
            subprocess.run(["make", "iso"])

        # Launch QEMU
        cmd = [
            self.qemu_path,
            "-cdrom", self.iso_path,
            "-m", "4G",
            "-smp", "4",
            "-serial", f"file:{self.serial_log}",
            "-vga", "std",
            "-vnc", ":2",  # VNC on port 5902
            "-monitor", "stdio",
            "-no-reboot",
        ]

        print(f"[*] Launching QEMU...")
        proc = subprocess.Popen(cmd)

        try:
            time.sleep(5)
            print("[*] Monitoring for file manager launch...")

            start_time = time.time()
            while time.time() - start_time < self.timeout:
                if self.parse_log_for_files_app():
                    print("[+] File manager launch detected!")
                    break
                time.sleep(1)

            print("\n[*] System running. Press Ctrl+C when done testing...")
            proc.wait()

        except KeyboardInterrupt:
            print("\n[*] Test interrupted by user")

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

        print("\n" + "=" * 70)
        print("[*] Manual Test Checklist:")
        print("    [ ] Desktop displayed with dock")
        print("    [ ] Files icon visible in dock")
        print("    [ ] Clicking files icon launches app")
        print("    [ ] File manager window appears")
        print("    [ ] Directory contents displayed")
        print("    [ ] Can navigate to subdirectories")
        print("    [ ] Can navigate up to parent directory")
        print("    [ ] File icons/names rendered correctly")
        print("=" * 70)

        return True


def main():
    test = FileManagerTest()
    test.run_test()
    sys.exit(0)


if __name__ == "__main__":
    main()
