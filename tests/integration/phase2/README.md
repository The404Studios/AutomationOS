# AutomationOS Phase 2 Integration Tests

Comprehensive integration tests for Phase 2 Security & Isolation mechanisms.

## Overview

Phase 2 implements 7 major security subsystems that must work together:

1. **Capability-based Security**: Fine-grained permissions (file, network, device, IPC)
2. **Namespace Isolation**: PID, Mount, Network, IPC, UTS namespaces
3. **Mandatory Access Control (MAC)**: Label-based security policies
4. **Resource Limits**: CPU, memory, I/O bandwidth limits
5. **Sandbox Enforcement**: Syscall filtering and isolation
6. **Audit Logging**: Comprehensive security event logging
7. **Secure Boot**: Kernel and module signature verification

These integration tests verify that all mechanisms work correctly both individually and together.

## Test Structure

```
tests/integration/phase2/
├── __init__.py                           # Package init
├── README.md                             # This file
├── test_framework.py                     # Common test utilities
├── test_scenario_web_server.py          # Scenario 1: Web Server Sandbox
├── test_scenario_untrusted_binary.py    # Scenario 2: Untrusted Binary
├── test_scenario_container.py           # Scenario 3: Container Isolation
└── test_full_stack_security.py          # Scenario 4: Full Stack Integration
```

## Test Scenarios

### Scenario 1: Web Server Sandbox

Tests a realistic web server with:
- MAC label: `web_server_t`
- Capabilities: `FILE_READ` (/var/www/*), `NETWORK_BIND` (port 80)
- Network namespace isolation
- Syscall filter (whitelist)
- Standard resource limits

**What it tests:**
- Can serve files from allowed directories
- Cannot read sensitive files (/etc/shadow)
- Cannot bind unauthorized ports (port 22)
- Cannot access other processes
- All violations are audited

**Run it:**
```bash
python3 tests/integration/phase2/test_scenario_web_server.py
```

---

### Scenario 2: Untrusted Binary Sandbox

Tests maximum security isolation for untrusted executables:
- MAC label: `untrusted_t`
- Capabilities: None (zero capabilities)
- All 5 namespace types (full isolation)
- Minimal syscall whitelist
- Strict resource limits (1GB RAM, 10% CPU)

**What it tests:**
- Cannot escape sandbox via filesystem
- Cannot escape via /proc
- Cannot escape via device nodes
- Cannot fork bomb
- Cannot exhaust system resources
- All known container escape techniques blocked

**Run it:**
```bash
python3 tests/integration/phase2/test_scenario_untrusted_binary.py
```

---

### Scenario 3: Container-Like Isolation

Tests Docker-like container isolation:
- All 5 namespace types (PID, Mount, Network, IPC, UTS)
- Per-container capabilities
- Per-container MAC policies
- Per-container resource limits
- Isolated process trees

**What it tests:**
- Each container has its own PID 1
- Containers cannot see each other's processes
- Separate network stacks per container
- IPC isolation between containers
- Custom hostname per container
- Nested containers work correctly

**Run it:**
```bash
python3 tests/integration/phase2/test_scenario_container.py
```

---

### Scenario 4: Full Stack Security Integration

Comprehensive integration tests covering all mechanisms:

**Test Suites:**
1. Security Subsystem Initialization
2. Capability + MAC Interaction
3. Namespace + Sandbox Interaction
4. Performance Overhead Measurement
5. Stress Testing (1000s of processes, millions of checks)
6. Security Boundary Enforcement

**What it tests:**
- All subsystems initialize correctly
- Mechanisms work together (capability + MAC + namespace)
- Performance overhead < 15%
- Stability under high load
- No security bypass paths

**Run it:**
```bash
python3 tests/integration/phase2/test_full_stack_security.py
```

## Running Tests

### Prerequisites

1. **Python 3.8+**: All tests are written in Python 3
2. **QEMU**: qemu-system-x86_64 must be installed and in PATH
3. **AutomationOS ISO**: Run `make iso` to build the bootable ISO

### Quick Start

```bash
# Check prerequisites
python3 tests/integration/phase2/test_framework.py

# Run all tests
python3 scripts/run_phase2_tests.py

# Run specific scenario
python3 scripts/run_phase2_tests.py --scenario 1

# Verbose output
python3 scripts/run_phase2_tests.py --verbose
```

### Automated Test Runner

Use the cross-platform test runner:

```bash
# Linux/macOS
./scripts/run-phase2-tests.sh

# Windows/cross-platform
python3 scripts/run_phase2_tests.py
```

Test results are saved to:
- **Logs**: `build/test-reports/*.log`
- **Report**: `build/test-reports/phase2-test-report.md`

## Test Framework

### QEMURunner

Manages QEMU test environment:

```python
with QEMURunner(iso_path, timeout=30, verbose=True) as qemu:
    qemu.wait_for_boot()
    output = qemu.get_serial_output()
    assert '[CAP]' in output  # Check capability system initialized
```

Features:
- Automatic QEMU lifecycle management
- Serial output capture
- Timeout handling
- KVM acceleration (when available)

### IntegrationTest

Base class for tests:

```python
class MyTest(IntegrationTest):
    def run(self):
        iso_path = find_iso()
        with QEMURunner(iso_path) as qemu:
            output = qemu.get_serial_output()
            
            # Assertions
            self.assert_contains(output, '[CAP]', 'Capability system initialized')
            self.assert_not_contains(output, 'PANIC', 'No kernel panics')
            
        return self.print_summary()
```

Features:
- Automatic result tracking
- Colored output
- Summary generation
- Timing information

### TestSuite

Collection of tests:

```python
suite = TestSuite("My Test Suite")
suite.add_test(Test1())
suite.add_test(Test2())
suite.run_all()  # Returns True if all passed
```

## Expected Test Output

### During Boot

```
[PMM] Physical Memory Manager initialized
[VMM] Virtual Memory Manager initialized
[HEAP] Kernel heap initialized
[SCHED] Scheduler initialized
[CAP] Capability system initialized
[NS] Namespace system initialized
  PID: 1, Mount: 1, Net: 1, IPC: 1, UTS: 1
[MAC] MAC policy engine initialized
[RLIMIT] Resource limit system initialized
[AUDIT] Audit system initialized
[KERNEL] AutomationOS Phase 2 ready
```

### During Tests

```
[CAP] Process 42 granted FILE_READ for /var/www/index.html
[CAP] Process 42 DENIED FILE_READ for /etc/shadow
[MAC] ALLOWED: web_server_t -> file_t (FILE_READ)
[MAC] DENIED: web_server_t -> shadow_t (FILE_READ)
[AUDIT] Event 1234: MAC_DENIED pid=42 path=/etc/shadow
[NS] Created namespace (PID: 2, parent: 1, level: 1)
[RLIMIT] Process 42 hit memory limit (1GB), allocation denied
```

## Performance Targets

| Metric | Target | Measured | Status |
|--------|--------|----------|--------|
| Capability check | < 100ns | TBD | 🟡 Pending |
| MAC check | < 200ns | TBD | 🟡 Pending |
| Namespace lookup | < 50ns | TBD | 🟡 Pending |
| Total syscall overhead | < 15% | TBD | 🟡 Pending |
| Context switch | < 500ns | TBD | 🟡 Pending |

## Troubleshooting

### Test Fails to Find ISO

**Error**: `AutomationOS.iso not found`

**Solution**:
```bash
cd /path/to/AutomationOS
make iso
```

### QEMU Not Found

**Error**: `qemu-system-x86_64 not found`

**Solution**:
```bash
# Ubuntu/Debian
sudo apt install qemu-system-x86

# macOS
brew install qemu

# Windows
# Download from https://www.qemu.org/download/
```

### Test Timeout

**Error**: `Test timeout (> 5 minutes)`

**Cause**: Kernel panic, infinite loop, or deadlock

**Solution**:
1. Check serial output: `cat build/test-reports/<test>.log`
2. Look for `PANIC`, `DEADLOCK`, or error messages
3. Run with `--verbose` for more details

### All Tests Show "Not Initialized"

**Cause**: Phase 2 kernel code not compiled or linked

**Solution**:
```bash
make clean
make kernel
make iso
python3 scripts/run_phase2_tests.py
```

## Contributing

When adding new tests:

1. Create test file in `tests/integration/phase2/`
2. Inherit from `IntegrationTest`
3. Implement `run()` method
4. Use `assert_*` methods for checks
5. Add to test runner script
6. Update this README

Example:

```python
class MyNewTest(IntegrationTest):
    def __init__(self):
        super().__init__(
            name="My New Test",
            description="Test description here"
        )
    
    def run(self) -> bool:
        # Test implementation
        iso_path = find_iso()
        with QEMURunner(iso_path) as qemu:
            output = qemu.get_serial_output()
            self.assert_contains(output, '[MY_SUBSYSTEM]', 'My subsystem initialized')
        return self.print_summary()
```

## Documentation

- **Test Report**: `docs/phase2/INTEGRATION_TEST_REPORT.md`
- **Phase 2 Plan**: `docs/superpowers/plans/2026-05-26-phase2-security-isolation.md`
- **Architecture**: `docs/ARCHITECTURE.md`
- **API Reference**: `docs/API_REFERENCE.md`

## Test Coverage

Current coverage (Phase 2 pre-release):

| Component | Unit Tests | Integration Tests | Coverage |
|-----------|-----------|-------------------|----------|
| Capabilities | ✓ Planned | ✓ Implemented | 85% target |
| Namespaces | ✓ Planned | ✓ Implemented | 85% target |
| MAC | ✓ Planned | ✓ Implemented | 85% target |
| Sandbox | ✓ Planned | ✓ Implemented | 85% target |
| Resource Limits | ✓ Planned | ✓ Implemented | 85% target |
| Audit | ✓ Planned | ✓ Implemented | 85% target |
| Secure Boot | ⏳ Planned | ⏳ Planned | Target: 80% |

## License

Part of AutomationOS project. See top-level LICENSE file.

## Support

For issues or questions:
1. Check documentation: `docs/phase2/`
2. Review test logs: `build/test-reports/`
3. Check kernel serial output for error messages

---

**Last Updated**: 2026-05-26  
**Test Suite Version**: 0.1.0  
**Compatible with**: AutomationOS Phase 2 Pre-Release
