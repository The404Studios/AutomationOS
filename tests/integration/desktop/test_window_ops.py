#!/usr/bin/env python3
"""
AutomationOS Window Operations Integration Test
Agent 12: Integration Test Lead

Tests window management operations:
- Window creation/destruction
- Window move via drag
- Window resize
- Window focus
- Window decorations
"""

import sys
import os
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


class WindowOpsTest:
    """Test window manager operations"""

    def __init__(self, qemu_path=None, iso_path=None):
        self.qemu_path = qemu_path or self._find_qemu()
        self.iso_path = iso_path or "build/automationos.iso"
        self.serial_log = "build/test_window_ops.log"
        self.timeout = 60

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

    def parse_log_for_wm(self):
        """Check for window manager events"""
        if not os.path.exists(self.serial_log):
            return {}

        with open(self.serial_log, 'r') as f:
            log = f.read()

        events = {
            "wm_init": "window manager" in log.lower() or "wm" in log.lower(),
            "window_created": "window created" in log.lower() or "create_window" in log.lower(),
            "window_moved": "window moved" in log.lower() or "move_window" in log.lower(),
            "window_focused": "focus" in log.lower(),
        }

        return events

    def run_test(self):
        """Run window operations test"""
        print("=" * 70)
        print("AutomationOS Window Operations Integration Test")
        print("Agent 12: Integration Test Lead")
        print("=" * 70)

        print("\n[*] This test validates window manager functionality:")
        print("    1. Windows have title bars and decorations")
        print("    2. Windows can be moved by dragging title bar")
        print("    3. Windows can be resized (if implemented)")
        print("    4. Clicking a window brings it to focus")
        print("    5. Window close button works")
        print("    6. Multiple windows can coexist")

        print(f"\n[*] Connect to VNC at localhost:5904 to interact")
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
            "-vnc", ":4",
            "-usb",
            "-device", "usb-tablet",
            "-monitor", "stdio",
            "-no-reboot",
        ]

        print(f"[*] Launching QEMU...")
        proc = subprocess.Popen(cmd)

        try:
            time.sleep(5)
            print("[*] Monitoring for window manager events...")

            start_time = time.time()
            while time.time() - start_time < 10:
                events = self.parse_log_for_wm()
                if events.get("wm_init"):
                    print("[+] Window manager initialized")
                    break
                time.sleep(1)

            print("\n[*] Test Procedure:")
            print("    1. Launch terminal and file manager")
            print("    2. Try to move terminal window")
            print("    3. Click on file manager to focus it")
            print("    4. Try to close terminal window")
            print("    5. Launch another terminal")
            print("    6. Verify both windows are visible")

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
        print("    [ ] Windows have title bars")
        print("    [ ] Windows have close button (X)")
        print("    [ ] Windows have minimize/maximize buttons")
        print("    [ ] Can drag window by title bar")
        print("    [ ] Window follows mouse during drag")
        print("    [ ] Window stays at new position after drop")
        print("    [ ] Clicking window brings it to front")
        print("    [ ] Focused window has different decoration")
        print("    [ ] Can resize window by dragging edges/corners")
        print("    [ ] Close button actually closes window")
        print("    [ ] Multiple windows don't overlap incorrectly")
        print("=" * 70)

        return True


def main():
    test = WindowOpsTest()
    test.run_test()
    sys.exit(0)


if __name__ == "__main__":
    main()
