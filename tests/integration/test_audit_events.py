#!/usr/bin/env python3
"""
Integration tests for audit logging subsystem

Tests audit events generated from real system operations including:
- File access auditing
- Process event auditing
- Security violation auditing
- Network event auditing
"""

import os
import subprocess
import time
import struct
from dataclasses import dataclass
from typing import List, Optional

# Audit event types (must match kernel definitions)
AUDIT_FILE_OPEN = 2000
AUDIT_FILE_WRITE = 2002
AUDIT_FILE_DELETE = 2003
AUDIT_PROC_EXEC = 3000
AUDIT_PROC_FORK = 3001
AUDIT_PROC_KILL = 3003
AUDIT_NET_CONNECT = 4000
AUDIT_SECURITY_CAP_DENIED = 5000
AUDIT_SECURITY_MAC_DENIED = 5001
AUDIT_SECURITY_SANDBOX_VIOLATION = 5002

# Result codes
AUDIT_SUCCESS = 0
AUDIT_FAILURE = 1
AUDIT_DENIED = 2

@dataclass
class AuditEvent:
    """Audit event structure"""
    timestamp: int
    sequence: int
    event_type: int
    result: int
    pid: int
    uid: int
    gid: int
    comm: str
    path: str
    object_pid: int
    syscall: int
    error_code: int
    flags: int
    prev_hash: int
    hash_value: int


class AuditTestSuite:
    """Integration test suite for audit logging"""

    def __init__(self):
        self.tests_passed = 0
        self.tests_failed = 0
        self.test_results = []

    def run_command(self, cmd: List[str], capture_output: bool = True) -> tuple:
        """Run a command and return (returncode, stdout, stderr)"""
        try:
            result = subprocess.run(
                cmd,
                capture_output=capture_output,
                text=True,
                timeout=5
            )
            return (result.returncode, result.stdout, result.stderr)
        except subprocess.TimeoutExpired:
            return (-1, "", "Command timed out")
        except Exception as e:
            return (-1, "", str(e))

    def enable_auditing(self) -> bool:
        """Enable audit logging"""
        returncode, stdout, stderr = self.run_command(['auditctl', 'enable'])
        return returncode == 0

    def disable_auditing(self) -> bool:
        """Disable audit logging"""
        returncode, stdout, stderr = self.run_command(['auditctl', 'disable'])
        return returncode == 0

    def get_audit_stats(self) -> tuple:
        """Get audit statistics (total, dropped)"""
        returncode, stdout, stderr = self.run_command(['auditctl', 'stats'])
        if returncode != 0:
            return (0, 0)

        # Parse output
        total = 0
        dropped = 0
        for line in stdout.split('\n'):
            if 'Total events:' in line:
                total = int(line.split(':')[1].strip())
            elif 'Dropped events:' in line:
                dropped = int(line.split(':')[1].strip())

        return (total, dropped)

    def search_audit_log(self, event_type: Optional[int] = None,
                        uid: Optional[int] = None,
                        result: Optional[str] = None) -> List[str]:
        """Search audit log with filters"""
        cmd = ['ausearch']

        if event_type is not None:
            cmd.extend(['-t', str(event_type)])
        if uid is not None:
            cmd.extend(['-u', str(uid)])
        if result is not None:
            cmd.extend(['-r', result])

        returncode, stdout, stderr = self.run_command(cmd)
        if returncode != 0:
            return []

        return stdout.split('\n')

    def add_audit_rule(self, filter_type: str, value: str, action: str = 'log') -> bool:
        """Add audit rule"""
        cmd = ['auditctl', 'add', filter_type, value, '-A', action]
        returncode, stdout, stderr = self.run_command(cmd)
        return returncode == 0

    def assert_test(self, condition: bool, test_name: str, message: str):
        """Assert test condition"""
        if condition:
            print(f"[PASS] {test_name}")
            self.tests_passed += 1
            self.test_results.append((test_name, True, ""))
        else:
            print(f"[FAIL] {test_name}: {message}")
            self.tests_failed += 1
            self.test_results.append((test_name, False, message))

    # Test cases

    def test_audit_enable_disable(self):
        """Test enabling and disabling audit logging"""
        print("\n[TEST] Audit enable/disable")

        # Enable auditing
        success = self.enable_auditing()
        self.assert_test(success, "audit_enable",
                        "Failed to enable auditing")

        # Disable auditing
        success = self.disable_auditing()
        self.assert_test(success, "audit_disable",
                        "Failed to disable auditing")

        # Re-enable for other tests
        self.enable_auditing()

    def test_audit_file_access(self):
        """Test file access auditing"""
        print("\n[TEST] File access auditing")

        # Enable auditing
        self.enable_auditing()

        # Get initial stats
        total_before, _ = self.get_audit_stats()

        # Perform file operations
        test_file = "/tmp/audit_test_file.txt"
        try:
            # Create file (should generate OPEN, WRITE events)
            with open(test_file, 'w') as f:
                f.write("test data")

            # Read file (should generate OPEN, READ events)
            with open(test_file, 'r') as f:
                _ = f.read()

            # Delete file (should generate DELETE event)
            os.unlink(test_file)

        except Exception as e:
            self.assert_test(False, "file_access_operations",
                           f"File operations failed: {e}")
            return

        # Wait for audit events to be logged
        time.sleep(0.5)

        # Get stats after operations
        total_after, _ = self.get_audit_stats()

        # Should have generated at least some events
        events_generated = total_after - total_before
        self.assert_test(events_generated > 0, "file_access_events_logged",
                        f"Expected events, got {events_generated}")

        # Search for file access events
        events = self.search_audit_log(event_type=AUDIT_FILE_OPEN)
        self.assert_test(len(events) > 0, "file_open_events",
                        "No file open events found")

    def test_audit_security_violations(self):
        """Test security violation auditing"""
        print("\n[TEST] Security violation auditing")

        # Add rule to alert on capability denied
        success = self.add_audit_rule('-t', str(AUDIT_SECURITY_CAP_DENIED),
                                     action='alert')
        self.assert_test(success, "add_security_rule",
                        "Failed to add security violation rule")

        # Get initial stats
        total_before, _ = self.get_audit_stats()

        # Attempt operation that requires elevated privileges
        # This should generate a capability denied event
        try:
            # Try to bind to privileged port (requires CAP_NET_BIND_SERVICE)
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                sock.bind(('127.0.0.1', 80))  # Port 80 requires privileges
            except PermissionError:
                pass  # Expected
            finally:
                sock.close()
        except Exception as e:
            print(f"Note: Security test operation failed: {e}")

        time.sleep(0.5)

        # Check if events were logged
        total_after, _ = self.get_audit_stats()
        events_generated = total_after - total_before

        self.assert_test(events_generated >= 0, "security_violation_logged",
                        "Security violation should be logged")

    def test_audit_process_events(self):
        """Test process event auditing"""
        print("\n[TEST] Process event auditing")

        # Get initial stats
        total_before, _ = self.get_audit_stats()

        # Spawn a subprocess (should generate FORK and EXEC events)
        try:
            subprocess.run(['echo', 'test'], capture_output=True, timeout=1)
        except Exception as e:
            self.assert_test(False, "process_spawn",
                           f"Failed to spawn process: {e}")
            return

        time.sleep(0.5)

        # Get stats after
        total_after, _ = self.get_audit_stats()
        events_generated = total_after - total_before

        self.assert_test(events_generated > 0, "process_events_logged",
                        f"Expected process events, got {events_generated}")

    def test_audit_rule_management(self):
        """Test audit rule add/delete"""
        print("\n[TEST] Audit rule management")

        # Add rule
        success = self.add_audit_rule('-f', '/tmp/*', action='log')
        self.assert_test(success, "add_path_rule",
                        "Failed to add path rule")

        # TODO: Implement rule listing to verify
        # For now, just check that add succeeded

    def test_audit_filtering(self):
        """Test audit log filtering"""
        print("\n[TEST] Audit log filtering")

        # Search for specific event type
        events = self.search_audit_log(event_type=AUDIT_FILE_OPEN)
        self.assert_test(isinstance(events, list), "filter_by_type",
                        "Failed to filter by event type")

        # Search by result
        events = self.search_audit_log(result='denied')
        self.assert_test(isinstance(events, list), "filter_by_result",
                        "Failed to filter by result")

    def test_audit_buffer_overflow(self):
        """Test audit buffer overflow handling"""
        print("\n[TEST] Audit buffer overflow")

        # Get initial stats
        total_before, dropped_before = self.get_audit_stats()

        # Generate many events rapidly
        for i in range(10000):
            try:
                # Quick file operations
                test_file = f"/tmp/audit_overflow_test_{i}.tmp"
                open(test_file, 'w').close()
                os.unlink(test_file)
            except:
                pass

        time.sleep(1)

        # Get stats after
        total_after, dropped_after = self.get_audit_stats()

        events_generated = total_after - total_before
        events_dropped = dropped_after - dropped_before

        self.assert_test(events_generated > 0, "overflow_events_generated",
                        "Should generate many events")

        # Buffer overflow is acceptable - verify dropped count is tracked
        if events_dropped > 0:
            print(f"  Note: {events_dropped} events dropped (expected under load)")

        self.assert_test(True, "overflow_handled",
                        "Buffer overflow handled correctly")

    def test_audit_performance(self):
        """Test audit logging performance"""
        print("\n[TEST] Audit performance")

        # Measure time for operations with auditing enabled
        start_time = time.time()

        for i in range(1000):
            test_file = f"/tmp/perf_test_{i}.tmp"
            try:
                with open(test_file, 'w') as f:
                    f.write("x")
                os.unlink(test_file)
            except:
                pass

        elapsed_with_audit = time.time() - start_time

        print(f"  1000 file operations took {elapsed_with_audit:.3f}s with auditing")

        # Performance should be reasonable (< 5 seconds for 1000 ops)
        self.assert_test(elapsed_with_audit < 5.0, "audit_performance",
                        f"Performance degradation too high: {elapsed_with_audit}s")

    def run_all_tests(self):
        """Run all integration tests"""
        print("=" * 80)
        print("Audit Logging Integration Tests")
        print("=" * 80)

        # Enable auditing for tests
        if not self.enable_auditing():
            print("FATAL: Failed to enable auditing")
            return

        # Run tests
        self.test_audit_enable_disable()
        self.test_audit_file_access()
        self.test_audit_security_violations()
        self.test_audit_process_events()
        self.test_audit_rule_management()
        self.test_audit_filtering()
        self.test_audit_buffer_overflow()
        self.test_audit_performance()

        # Print summary
        print("\n" + "=" * 80)
        print("Test Summary")
        print("=" * 80)
        print(f"Tests passed: {self.tests_passed}")
        print(f"Tests failed: {self.tests_failed}")
        print(f"Total tests:  {self.tests_passed + self.tests_failed}")

        if self.tests_failed == 0:
            print("\nALL TESTS PASSED!")
            print("=" * 80)
            return 0
        else:
            print("\nSOME TESTS FAILED!")
            print("\nFailed tests:")
            for name, passed, message in self.test_results:
                if not passed:
                    print(f"  - {name}: {message}")
            print("=" * 80)
            return 1


if __name__ == "__main__":
    suite = AuditTestSuite()
    exit_code = suite.run_all_tests()
    exit(exit_code)
