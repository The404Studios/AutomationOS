#!/usr/bin/env python3
"""
Integration Test: Secure Boot Chain
Tests kernel and module signature verification
"""

import os
import sys
import subprocess
import tempfile
import shutil

class SecureBootTester:
    def __init__(self, project_root):
        self.project_root = project_root
        self.scripts_dir = os.path.join(project_root, 'scripts')
        self.build_dir = os.path.join(project_root, 'build')
        self.keys_dir = os.path.join(project_root, 'keys')
        self.test_dir = tempfile.mkdtemp(prefix='secboot_test_')

    def cleanup(self):
        """Cleanup test directory"""
        if os.path.exists(self.test_dir):
            shutil.rmtree(self.test_dir)

    def run_command(self, cmd, cwd=None, check=True):
        """Run shell command"""
        print(f"  $ {cmd}")
        result = subprocess.run(
            cmd, shell=True, cwd=cwd or self.project_root,
            capture_output=True, text=True
        )
        if check and result.returncode != 0:
            print(f"ERROR: Command failed: {cmd}")
            print(f"STDOUT: {result.stdout}")
            print(f"STDERR: {result.stderr}")
            sys.exit(1)
        return result

    def test_key_generation(self):
        """Test 1: Generate signing keys"""
        print("\n[TEST 1] Key Generation")
        print("-" * 60)

        # Remove existing keys
        if os.path.exists(self.keys_dir):
            shutil.rmtree(self.keys_dir)

        # Generate new keys
        keygen_script = os.path.join(self.scripts_dir, 'generate-keys.sh')
        self.run_command(f'bash {keygen_script}')

        # Check keys were created
        private_key = os.path.join(self.keys_dir, 'kernel-signing-key.pem')
        public_key = os.path.join(self.keys_dir, 'kernel-signing-key-pub.pem')
        header = os.path.join(self.project_root, 'boot', 'boot_pubkey.h')

        assert os.path.exists(private_key), "Private key not created"
        assert os.path.exists(public_key), "Public key not created"
        assert os.path.exists(header), "Public key header not created"

        print("✓ Keys generated successfully")
        return True

    def test_kernel_signing(self):
        """Test 2: Sign kernel binary"""
        print("\n[TEST 2] Kernel Signing")
        print("-" * 60)

        # Create dummy kernel for testing
        kernel_path = os.path.join(self.test_dir, 'kernel.elf')
        with open(kernel_path, 'wb') as f:
            f.write(b'KERNEL_CODE_HERE' * 1000)  # Dummy kernel data

        # Sign kernel
        sign_script = os.path.join(self.scripts_dir, 'sign-kernel.sh')
        signed_path = os.path.join(self.test_dir, 'kernel.elf.signed')
        self.run_command(f'bash {sign_script} {kernel_path} {signed_path}')

        # Check signed kernel exists
        assert os.path.exists(signed_path), "Signed kernel not created"

        # Verify signature header
        with open(signed_path, 'rb') as f:
            data = f.read()

        # Should be larger than original (signature appended)
        assert len(data) > 16000, "Signed kernel too small"

        print("✓ Kernel signed successfully")
        return True

    def test_kernel_signature_verification(self):
        """Test 3: Verify kernel signature"""
        print("\n[TEST 3] Kernel Signature Verification")
        print("-" * 60)

        signed_path = os.path.join(self.test_dir, 'kernel.elf.signed')
        verify_script = os.path.join(self.scripts_dir, 'verify-signature.py')

        result = self.run_command(f'python3 {verify_script} {signed_path}')

        assert result.returncode == 0, "Signature verification failed"

        print("✓ Kernel signature verified")
        return True

    def test_tampered_kernel_detection(self):
        """Test 4: Detect tampered kernel"""
        print("\n[TEST 4] Tampered Kernel Detection")
        print("-" * 60)

        signed_path = os.path.join(self.test_dir, 'kernel.elf.signed')
        tampered_path = os.path.join(self.test_dir, 'kernel_tampered.elf.signed')

        # Copy signed kernel
        shutil.copy(signed_path, tampered_path)

        # Tamper with kernel code (flip a byte in the middle)
        with open(tampered_path, 'r+b') as f:
            f.seek(1000)
            byte = f.read(1)
            f.seek(1000)
            f.write(bytes([byte[0] ^ 0xFF]))

        # Try to verify tampered kernel (should fail)
        verify_script = os.path.join(self.scripts_dir, 'verify-signature.py')
        result = self.run_command(
            f'python3 {verify_script} {tampered_path}',
            check=False
        )

        assert result.returncode != 0, "Tampered kernel verification should fail"

        print("✓ Tampered kernel detected")
        return True

    def test_module_signing(self):
        """Test 5: Sign kernel module"""
        print("\n[TEST 5] Module Signing")
        print("-" * 60)

        # Create dummy module
        module_path = os.path.join(self.test_dir, 'test_module.ko')
        with open(module_path, 'wb') as f:
            f.write(b'MODULE_CODE_HERE' * 500)

        # Sign module
        sign_script = os.path.join(self.scripts_dir, 'sign-module.sh')
        signed_path = os.path.join(self.test_dir, 'test_module_signed.ko')
        self.run_command(f'bash {sign_script} {module_path} {signed_path}')

        assert os.path.exists(signed_path), "Signed module not created"

        print("✓ Module signed successfully")
        return True

    def test_module_signature_verification(self):
        """Test 6: Verify module signature"""
        print("\n[TEST 6] Module Signature Verification")
        print("-" * 60)

        signed_path = os.path.join(self.test_dir, 'test_module_signed.ko')
        verify_script = os.path.join(self.scripts_dir, 'verify-signature.py')

        result = self.run_command(f'python3 {verify_script} {signed_path}')

        assert result.returncode == 0, "Module signature verification failed"

        print("✓ Module signature verified")
        return True

    def test_unsigned_module_rejection(self):
        """Test 7: Reject unsigned module"""
        print("\n[TEST 7] Unsigned Module Rejection")
        print("-" * 60)

        # Create unsigned module
        unsigned_path = os.path.join(self.test_dir, 'unsigned_module.ko')
        with open(unsigned_path, 'wb') as f:
            f.write(b'UNSIGNED_MODULE' * 100)

        # Try to verify unsigned module (should fail)
        verify_script = os.path.join(self.scripts_dir, 'verify-signature.py')
        result = self.run_command(
            f'python3 {verify_script} {unsigned_path}',
            check=False
        )

        assert result.returncode != 0, "Unsigned module should be rejected"

        print("✓ Unsigned module rejected")
        return True

    def run_all_tests(self):
        """Run all secure boot tests"""
        print("\n" + "=" * 60)
        print("  Secure Boot Chain Integration Tests")
        print("=" * 60)

        tests = [
            self.test_key_generation,
            self.test_kernel_signing,
            self.test_kernel_signature_verification,
            self.test_tampered_kernel_detection,
            self.test_module_signing,
            self.test_module_signature_verification,
            self.test_unsigned_module_rejection,
        ]

        passed = 0
        failed = 0

        for test in tests:
            try:
                if test():
                    passed += 1
            except Exception as e:
                print(f"✗ Test failed: {e}")
                failed += 1

        print("\n" + "=" * 60)
        print(f"  Results: {passed} passed, {failed} failed")
        print("=" * 60)

        return failed == 0

def main():
    if len(sys.argv) < 2:
        print("Usage: test_secure_boot.py <project-root>")
        sys.exit(1)

    project_root = sys.argv[1]

    tester = SecureBootTester(project_root)
    try:
        success = tester.run_all_tests()
        sys.exit(0 if success else 1)
    finally:
        tester.cleanup()

if __name__ == '__main__':
    main()
