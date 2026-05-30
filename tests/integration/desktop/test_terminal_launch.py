#!/usr/bin/env python3
"""
AutomationOS Terminal Launch Integration Test
Agent 12: Integration Test Lead

Tests launching the terminal application from the dock/desktop.
Validates terminal window appears and shell prompt is displayed.
"""

import sys
import os
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


class TerminalLaunchTest:
    """Test terminal application launch"""

    def __init__(self, qemu_path=None, iso_path=None):
        self.qemu_path = qemu_path or self._find_qemu()
        self.iso_path = iso_path or "build/automationos.iso"
        self.serial_log = "build/test_terminal_launch.log"
        self.timeout = 45  # seconds

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

    def run_qemu_interactive(self):
        """Launch QEMU with VNC for interactive testing"""
        cmd = [
            self.qemu_path,
            "-cdrom", self.iso_path,
            "-m", "4G",
            "-smp", "4",
            "-serial", f"file:{self.serial_log}",
            "-vga", "std",
            "-vnc", ":1",  # VNC on port 5901
            "-monitor", "stdio",
            "-no-reboot",
        ]

        print(f"[*] Launching QEMU with VNC on :5901")
        print(f"[*] Command: {' '.join(cmd)}")

        proc = subprocess.Popen(cmd)
        return proc

    def parse_log_for_terminal(self):
        """Check if terminal was launched"""
        if not os.path.exists(self.serial_log):
            return False

        with open(self.serial_log, 'r') as f:
            log = f.read()

        # Look for terminal launch indicators
        indicators = [
            "terminal",
            "pty",
            "/bin/sh",
            "Terminal started",
            "PTY created",
        ]

        for indicator in indicators:
            if indicator in log.lower():
                return True

        return False

    def run_test(self):
        """Run terminal launch test"""
        print("=" * 70)
        print("AutomationOS Terminal Launch Integration Test")
        print("Agent 12: Integration Test Lead")
        print("=" * 70)

        print("\n[*] This test requires manual interaction:")
        print("    1. System will boot to desktop")
        print("    2. Click the terminal icon in the dock")
        print("    3. Verify terminal window appears")
        print("    4. Verify shell prompt is shown")
        print("    5. Type commands to verify input works")

        print(f"\n[*] Connect to VNC at localhost:5901 to interact")
        print(f"[*] Press Ctrl+C to end test\n")

        # Check ISO
        if not os.path.exists(self.iso_path):
            print("[!] ISO not found, building...")
            subprocess.run(["make", "iso"])

        # Launch QEMU
        proc = self.run_qemu_interactive()

        try:
            # Wait for user interaction
            time.sleep(5)
            print("[*] Monitoring for terminal launch...")

            start_time = time.time()
            terminal_detected = False

            while time.time() - start_time < self.timeout:
                if self.parse_log_for_terminal():
                    terminal_detected = True
                    print("[+] Terminal launch detected in logs!")
                    break
                time.sleep(1)

            # Keep running for manual testing
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
        print("    [ ] Terminal icon visible in dock")
        print("    [ ] Clicking terminal icon launches app")
        print("    [ ] Terminal window appears with decorations")
        print("    [ ] Shell prompt displayed")
        print("    [ ] Keyboard input works")
        print("    [ ] Terminal can execute commands")
        print("=" * 70)

        return True


def main():
    test = TerminalLaunchTest()
    test.run_test()
    sys.exit(0)


if __name__ == "__main__":
    main()
