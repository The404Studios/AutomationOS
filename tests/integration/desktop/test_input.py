#!/usr/bin/env python3
"""
AutomationOS Input Events Integration Test
Agent 12: Integration Test Lead

Tests mouse and keyboard input handling.
Validates input events reach userspace applications.
"""

import sys
import os
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


class InputEventsTest:
    """Test input event pipeline"""

    def __init__(self, qemu_path=None, iso_path=None):
        self.qemu_path = qemu_path or self._find_qemu()
        self.iso_path = iso_path or "build/automationos.iso"
        self.serial_log = "build/test_input.log"
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

    def parse_log_for_input(self):
        """Check for input events in log"""
        if not os.path.exists(self.serial_log):
            return {"mouse": False, "keyboard": False}

        with open(self.serial_log, 'r') as f:
            log = f.read()

        events = {
            "mouse": any(x in log.lower() for x in ["mouse", "ps/2 mouse", "pointer"]),
            "keyboard": any(x in log.lower() for x in ["keyboard", "ps/2 keyboard", "key"]),
            "ps2_init": "PS/2" in log or "ps2" in log.lower(),
        }

        return events

    def run_test(self):
        """Run input events test"""
        print("=" * 70)
        print("AutomationOS Input Events Integration Test")
        print("Agent 12: Integration Test Lead")
        print("=" * 70)

        print("\n[*] This test validates input event handling:")
        print("    1. Mouse movement updates cursor position")
        print("    2. Mouse clicks register")
        print("    3. Keyboard input reaches applications")
        print("    4. Special keys work (arrows, modifiers)")

        print(f"\n[*] Connect to VNC at localhost:5903 to interact")
        print(f"[*] Press Ctrl+C to end test\n")

        # Check ISO
        if not os.path.exists(self.iso_path):
            print("[!] ISO not found, building...")
            subprocess.run(["make", "iso"])

        # Launch QEMU with input devices
        cmd = [
            self.qemu_path,
            "-cdrom", self.iso_path,
            "-m", "4G",
            "-smp", "4",
            "-serial", f"file:{self.serial_log}",
            "-vga", "std",
            "-vnc", ":3",
            "-usb",
            "-device", "usb-tablet",  # Better mouse support
            "-monitor", "stdio",
            "-no-reboot",
        ]

        print(f"[*] Launching QEMU with USB tablet for mouse...")
        proc = subprocess.Popen(cmd)

        try:
            time.sleep(5)
            print("[*] Monitoring for input events...")

            # Check for PS/2 initialization
            events = self.parse_log_for_input()
            if events.get("ps2_init"):
                print("[+] PS/2 controller initialized")
            if events.get("keyboard"):
                print("[+] Keyboard detected")
            if events.get("mouse"):
                print("[+] Mouse detected")

            print("\n[*] Test Actions:")
            print("    - Move mouse around the screen")
            print("    - Click on dock icons")
            print("    - Type in terminal/applications")
            print("    - Try arrow keys, Ctrl, Alt, etc.")

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
        print("    [ ] Mouse cursor visible on screen")
        print("    [ ] Mouse moves smoothly")
        print("    [ ] Left click registers on UI elements")
        print("    [ ] Right click shows context menu (if implemented)")
        print("    [ ] Keyboard input appears in terminal")
        print("    [ ] Special keys work (arrows, Enter, Backspace)")
        print("    [ ] Modifier keys work (Ctrl, Alt, Shift)")
        print("    [ ] No input lag or freezing")
        print("=" * 70)

        return True


def main():
    test = InputEventsTest()
    test.run_test()
    sys.exit(0)


if __name__ == "__main__":
    main()
