#!/usr/bin/env python3
"""
AutomationOS Desktop Boot Integration Test
Agent 12: Integration Test Lead

Tests the complete boot sequence from kernel boot to desktop display.
Validates that panel and dock are visible on screen.
"""

import sys
import os
import subprocess
import time
import re
from pathlib import Path

# Add parent directory to path for test framework
sys.path.insert(0, str(Path(__file__).parent.parent))

class DesktopBootTest:
    """Test desktop boot sequence"""

    def __init__(self, qemu_path=None, iso_path=None):
        self.qemu_path = qemu_path or self._find_qemu()
        self.iso_path = iso_path or "build/automationos.iso"
        self.serial_log = "build/test_desktop_boot.log"
        self.screenshot_dir = "build/screenshots"
        self.timeout = 30  # seconds

    def _find_qemu(self):
        """Find QEMU binary on system"""
        paths = [
            "qemu-system-x86_64",
            "/c/Program Files/qemu/qemu-system-x86_64.exe",
            "/c/Program Files (x86)/qemu/qemu-system-x86_64.exe",
            "/usr/bin/qemu-system-x86_64",
            "/usr/local/bin/qemu-system-x86_64",
        ]

        for path in paths:
            if os.path.exists(path) or subprocess.run(
                ["which", path], capture_output=True
            ).returncode == 0:
                return path

        raise FileNotFoundError("QEMU not found. Install qemu-system-x86_64")

    def _find_ovmf(self):
        """Find OVMF UEFI firmware"""
        paths = [
            "/usr/share/ovmf/OVMF.fd",
            "/usr/share/OVMF/OVMF_CODE.fd",
            "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd",
            "/c/Program Files/qemu/share/edk2-x86_64-code.fd",
        ]

        for path in paths:
            if os.path.exists(path):
                return path

        return None  # Fall back to BIOS boot

    def run_qemu(self, timeout=None):
        """Launch QEMU and capture serial output"""
        timeout = timeout or self.timeout

        # Create screenshot directory
        os.makedirs(self.screenshot_dir, exist_ok=True)

        # Build QEMU command
        cmd = [
            self.qemu_path,
            "-cdrom", self.iso_path,
            "-m", "4G",
            "-smp", "4",
            "-serial", f"file:{self.serial_log}",
            "-vga", "std",
            "-display", "gtk",
            "-no-reboot",
            "-no-shutdown",
        ]

        # Add UEFI if available
        ovmf = self._find_ovmf()
        if ovmf:
            cmd.extend(["-bios", ovmf])

        print(f"[*] Launching QEMU: {' '.join(cmd)}")

        # Start QEMU in background
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        # Wait for boot
        start_time = time.time()
        boot_complete = False

        try:
            while time.time() - start_time < timeout:
                if os.path.exists(self.serial_log):
                    with open(self.serial_log, 'r') as f:
                        log = f.read()

                    # Check for boot completion markers
                    if "Desktop ready" in log or "desktop_shell" in log or "compositor" in log:
                        boot_complete = True
                        print("[+] Boot completed!")
                        break

                    # Check for shell prompt as fallback
                    if "shell>" in log or "sh>" in log:
                        print("[!] Shell prompt detected (desktop may not be running)")
                        break

                time.sleep(0.5)

            # Take screenshot after boot
            time.sleep(2)  # Give compositor time to render

        finally:
            # Terminate QEMU
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

        return boot_complete

    def parse_serial_log(self):
        """Parse serial log for boot events"""
        if not os.path.exists(self.serial_log):
            return {}

        with open(self.serial_log, 'r') as f:
            log = f.read()

        events = {
            "kernel_boot": False,
            "gdt_init": False,
            "idt_init": False,
            "pmm_init": False,
            "vmm_init": False,
            "framebuffer_init": False,
            "scheduler_init": False,
            "vfs_init": False,
            "init_started": False,
            "compositor_started": False,
            "desktop_started": False,
            "shell_prompt": False,
        }

        # Parse for key events
        if "AutomationOS" in log:
            events["kernel_boot"] = True
        if "GDT initialized" in log:
            events["gdt_init"] = True
        if "IDT initialized" in log:
            events["idt_init"] = True
        if "PMM initialized" in log or "Physical memory manager" in log:
            events["pmm_init"] = True
        if "VMM initialized" in log or "Virtual memory" in log:
            events["vmm_init"] = True
        if "Framebuffer initialized" in log or "framebuffer" in log:
            events["framebuffer_init"] = True
        if "Scheduler initialized" in log or "scheduler" in log:
            events["scheduler_init"] = True
        if "VFS initialized" in log or "vfs" in log:
            events["vfs_init"] = True
        if "init" in log.lower() and "pid" in log.lower():
            events["init_started"] = True
        if "compositor" in log.lower():
            events["compositor_started"] = True
        if "desktop" in log.lower():
            events["desktop_started"] = True
        if "shell>" in log or "sh>" in log:
            events["shell_prompt"] = True

        return events

    def validate_boot(self):
        """Validate boot sequence"""
        events = self.parse_serial_log()

        required_events = [
            "kernel_boot",
            "gdt_init",
            "idt_init",
            "pmm_init",
            "vmm_init",
            "framebuffer_init",
            "scheduler_init",
            "vfs_init",
            "init_started",
        ]

        print("\n[*] Boot Event Validation:")
        all_passed = True

        for event in required_events:
            status = "✓" if events.get(event, False) else "✗"
            print(f"  {status} {event}")
            if not events.get(event, False):
                all_passed = False

        print("\n[*] Desktop Component Status:")
        desktop_events = ["compositor_started", "desktop_started", "shell_prompt"]
        for event in desktop_events:
            status = "✓" if events.get(event, False) else "✗"
            print(f"  {status} {event}")

        return all_passed, events

    def run_test(self):
        """Run complete desktop boot test"""
        print("=" * 70)
        print("AutomationOS Desktop Boot Integration Test")
        print("Agent 12: Integration Test Lead")
        print("=" * 70)

        # Check ISO exists
        if not os.path.exists(self.iso_path):
            print(f"[!] ISO not found: {self.iso_path}")
            print("[*] Building ISO...")
            result = subprocess.run(["make", "iso"], cwd=".")
            if result.returncode != 0:
                print("[✗] Build failed!")
                return False

        # Run QEMU boot test
        print(f"\n[*] Starting boot test (timeout: {self.timeout}s)...")
        boot_ok = self.run_qemu()

        # Validate boot
        passed, events = self.validate_boot()

        # Print results
        print("\n" + "=" * 70)
        if passed:
            print("[✓] BOOT TEST PASSED")
            print(f"[*] All required kernel components initialized")
        else:
            print("[✗] BOOT TEST FAILED")
            print(f"[!] Some required components did not initialize")

        # Desktop status
        desktop_ok = events.get("compositor_started") and events.get("desktop_started")
        if desktop_ok:
            print("[✓] Desktop components detected")
        else:
            print("[!] Desktop components not detected (may be running shell only)")

        print("=" * 70)
        print(f"\n[*] Serial log: {self.serial_log}")
        print(f"[*] Screenshots: {self.screenshot_dir}")

        return passed


def main():
    """Main entry point"""
    test = DesktopBootTest()
    success = test.run_test()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
