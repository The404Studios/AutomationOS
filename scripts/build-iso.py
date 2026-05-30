#!/usr/bin/env python3
"""
AutomationOS ISO Builder

Generates a bootable UEFI ISO image from built components.
Requires: xorriso

Usage:
    python3 scripts/build-iso.py
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path


class ISOBuilder:
    def __init__(self):
        self.root = Path(__file__).parent.parent
        self.build_dir = self.root / 'build'
        self.iso_dir = self.root / 'iso'
        self.iso_file = self.build_dir / 'AutomationOS.iso'

    def check_dependencies(self):
        """Check that required tools are available"""
        print("[ISO] Checking dependencies...")

        if not shutil.which('xorriso'):
            print("ERROR: xorriso not found. Install with:")
            print("  Ubuntu/Debian: sudo apt install xorriso")
            print("  Arch: sudo pacman -S libisoburn")
            print("  macOS: brew install xorriso")
            return False

        print("  ✓ xorriso found")
        return True

    def check_build_artifacts(self):
        """Verify that bootloader and kernel have been built"""
        print("[ISO] Checking build artifacts...")

        bootloader = self.build_dir / 'BOOTX64.EFI'
        kernel = self.build_dir / 'kernel.elf'
        initrd = self.build_dir / 'initrd.img'

        missing = []
        if not bootloader.exists():
            missing.append(str(bootloader))
        else:
            print(f"  ✓ Bootloader found: {bootloader}")

        if not kernel.exists():
            missing.append(str(kernel))
        else:
            print(f"  ✓ Kernel found: {kernel}")

        if not initrd.exists():
            print(f"  ⚠ Initrd not found (optional): {initrd}")
        else:
            print(f"  ✓ Initrd found: {initrd}")

        if missing:
            print("\nERROR: Missing required build artifacts:")
            for artifact in missing:
                print(f"  - {artifact}")
            print("\nRun 'make bootloader kernel' first")
            return False

        return True

    def create_iso_structure(self):
        """Create ISO directory structure"""
        print("[ISO] Creating ISO directory structure...")

        # Create directories
        efi_boot = self.iso_dir / 'EFI' / 'BOOT'
        boot = self.iso_dir / 'boot'

        efi_boot.mkdir(parents=True, exist_ok=True)
        boot.mkdir(parents=True, exist_ok=True)

        print(f"  ✓ Created {efi_boot}")
        print(f"  ✓ Created {boot}")

    def copy_files(self):
        """Copy bootloader and kernel to ISO directory"""
        print("[ISO] Copying files...")

        # Copy bootloader
        bootloader_src = self.build_dir / 'BOOTX64.EFI'
        bootloader_dst = self.iso_dir / 'EFI' / 'BOOT' / 'BOOTX64.EFI'
        shutil.copy(bootloader_src, bootloader_dst)
        print(f"  ✓ Copied bootloader: {bootloader_dst}")

        # Copy kernel
        kernel_src = self.build_dir / 'kernel.elf'
        kernel_dst = self.iso_dir / 'boot' / 'kernel.elf'
        shutil.copy(kernel_src, kernel_dst)
        print(f"  ✓ Copied kernel: {kernel_dst}")

        # Copy initrd (if exists)
        initrd_src = self.build_dir / 'initrd.img'
        if initrd_src.exists():
            initrd_dst = self.iso_dir / 'boot' / 'initrd.img'
            shutil.copy(initrd_src, initrd_dst)
            print(f"  ✓ Copied initrd: {initrd_dst}")
        else:
            print(f"  ⚠ No initrd found, skipping")

    def generate_iso(self):
        """Generate bootable ISO using xorriso"""
        print("[ISO] Generating ISO image...")

        cmd = [
            'xorriso',
            '-as', 'mkisofs',
            '-R',  # Rock Ridge (long filenames)
            '-J',  # Joliet (Windows compatibility)
            '-e', 'EFI/BOOT/BOOTX64.EFI',
            '-no-emul-boot',
            '-o', str(self.iso_file),
            str(self.iso_dir)
        ]

        try:
            result = subprocess.run(
                cmd,
                check=True,
                capture_output=True,
                text=True
            )
            print(f"  ✓ Generated ISO: {self.iso_file}")

            # Show ISO size
            size_mb = self.iso_file.stat().st_size / (1024 * 1024)
            print(f"  ✓ ISO size: {size_mb:.2f} MB")

            return True
        except subprocess.CalledProcessError as e:
            print(f"ERROR: xorriso failed with exit code {e.returncode}")
            print(f"STDOUT: {e.stdout}")
            print(f"STDERR: {e.stderr}")
            return False

    def build(self):
        """Main build process"""
        print("=" * 50)
        print("AutomationOS ISO Builder")
        print("=" * 50)
        print()

        # Check dependencies
        if not self.check_dependencies():
            return 1

        # Check build artifacts
        if not self.check_build_artifacts():
            return 1

        # Create ISO structure
        self.create_iso_structure()

        # Copy files
        self.copy_files()

        # Generate ISO
        if not self.generate_iso():
            return 1

        print()
        print("=" * 50)
        print("✓ ISO build complete!")
        print("=" * 50)
        print()
        print(f"ISO file: {self.iso_file}")
        print()
        print("Next steps:")
        print("  make qemu       # Run in QEMU")
        print("  make qemu-debug # Run with GDB debugging")
        print()

        return 0


def main():
    builder = ISOBuilder()
    return builder.build()


if __name__ == '__main__':
    sys.exit(main())
