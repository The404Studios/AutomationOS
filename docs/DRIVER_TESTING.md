# Driver Testing Guide

Comprehensive guide for AutomationOS driver testing framework and test suites.

## Table of Contents

1. [Overview](#overview)
2. [Test Framework Architecture](#test-framework-architecture)
3. [Running Tests](#running-tests)
4. [Driver Test Suites](#driver-test-suites)
5. [Writing New Tests](#writing-new-tests)
6. [Mock Device Framework](#mock-device-framework)
7. [Performance Benchmarking](#performance-benchmarking)
8. [Stress Testing](#stress-testing)
9. [Hardware Testing](#hardware-testing)
10. [Troubleshooting](#troubleshooting)

---

## Overview

The AutomationOS driver test suite provides comprehensive validation for all hardware drivers including storage (NVMe, AHCI), network (e1000), graphics (i915), audio (HDA), and USB (xHCI, HID).

### Test Coverage

- **Storage Drivers**: NVMe, AHCI (SATA)
  - Sequential/random I/O
  - Queue depth scaling (1-32)
  - Error injection and recovery
  - Hot-plug simulation
  - NCQ testing
  
- **Network Drivers**: e1000
  - Packet transmission/reception
  - Throughput testing
  - MTU sizes (standard and jumbo frames)
  - Link state changes
  - Packet loss detection

- **Graphics Drivers**: i915
  - Mode setting (resolutions, refresh rates)
  - Multi-monitor support (up to 4 displays)
  - Page flipping and VSync
  - Hardware cursor
  - Hot-plug detection

- **Audio Drivers**: HDA
  - Playback and recording
  - Multiple sample rates (8kHz - 192kHz)
  - Format support (16/24/32-bit)
  - Multiple simultaneous streams
  - Buffer underrun/overrun handling

- **USB Drivers**: xHCI, HID
  - Device enumeration
  - Bulk/interrupt/control transfers
  - HID devices (keyboard, mouse, gamepad)
  - Hot-plug stress testing (100+ cycles)
  - USB 2.0 and 3.0 support

### Success Criteria

- ✅ All tests pass
- ✅ No crashes in 24-hour stress tests
- ✅ No memory leaks detected
- ✅ Performance targets met:
  - Storage: > 1 GB/s sequential, > 100K IOPS random
  - Network: > 900 Mbps throughput
  - Graphics: 60+ FPS at 1080p
  - Audio: < 50ms latency

---

## Test Framework Architecture

### Core Components

```
tests/drivers/
├── driver_test_framework.h      # Framework API
├── driver_test_framework.c      # Framework implementation
├── driver_test_main.c           # Test runner
├── test_nvme.c                  # NVMe tests
├── test_ahci.c                  # AHCI tests
├── test_network.c               # Network tests
├── test_graphics.c              # Graphics tests
├── test_audio.c                 # Audio tests
└── test_usb.c                   # USB tests
```

### Framework Features

- **Test Organization**: Hierarchical test suites and cases
- **Mock Devices**: Software simulation of hardware
- **Timing Utilities**: Microsecond-precision timing
- **Memory Leak Detection**: Automatic tracking
- **DMA Buffer Simulation**: Virtual DMA buffers
- **IRQ Simulation**: Interrupt handling simulation
- **PCI Device Simulation**: Mock PCI configuration space

### Test Lifecycle

```
1. Framework Initialization
   └─> test_framework_init()

2. Test Registration
   └─> test_register_suite()
       └─> test_register_case()

3. Test Execution
   └─> test_run_all()
       ├─> Suite Setup
       ├─> For each test case:
       │   ├─> Execute test function
       │   └─> Collect results
       ├─> Suite Teardown
       └─> Print statistics

4. Cleanup
   └─> test_memory_check_leaks()
```

---

## Running Tests

### Build Tests

```bash
cd tests/drivers
make
```

### Run All Tests

```bash
./driver_test --all
```

### Run Specific Driver Tests

```bash
# Storage tests
./driver_test --storage

# Network tests
./driver_test --network

# Graphics tests
./driver_test --graphics

# Audio tests
./driver_test --audio

# USB tests
./driver_test --usb
```

### Run Specific Test Case

```bash
./driver_test --test nvme::sequential_read
./driver_test --test ahci::ncq_read
./driver_test --test e1000::throughput_1gbps
```

### Quick Tests (Skip Stress Tests)

```bash
./driver_test --all --quick
```

### Verbose Output

```bash
./driver_test --all --verbose
```

### Command-Line Options

```
Usage: driver_test [OPTIONS]

Options:
  -a, --all              Run all tests
  -s, --serial           Run serial driver tests
  --storage              Run storage driver tests (NVMe, AHCI)
  --network              Run network driver tests (e1000)
  --graphics             Run graphics driver tests (i915)
  --audio                Run audio driver tests (HDA)
  --usb                  Run USB driver tests (xHCI, HID)
  --stress               Run stress tests only
  --test <name>          Run specific test case
  -q, --quick            Run quick tests only (skip stress tests)
  -v, --verbose          Verbose output
  -h, --help             Show this help message
```

---

## Driver Test Suites

### NVMe Storage Driver Tests

**Test Cases** (18 total):

1. **Initialization Tests** (4)
   - Device detection
   - Controller initialization
   - Admin queue setup
   - I/O queue creation

2. **I/O Operation Tests** (4)
   - Sequential read
   - Sequential write
   - Random I/O
   - Large transfer (1MB)

3. **Queue Depth Tests** (2)
   - Queue depth = 1
   - Queue depth = 32

4. **Error Injection Tests** (3)
   - Timeout handling
   - Error recovery
   - DMA corruption detection

5. **Performance Tests** (2)
   - Sequential read bandwidth
   - Random IOPS

6. **Stress Tests** (1)
   - Mixed workload (1 second)

**Running NVMe Tests**:
```bash
./driver_test --storage --verbose
./driver_test --test nvme::random_io
```

### AHCI (SATA) Driver Tests

**Test Cases** (17 total):

1. **Initialization** (4)
   - Controller detection
   - HBA initialization
   - Port detection
   - Device identification

2. **I/O Operations** (3)
   - PIO read
   - DMA write
   - Multi-sector transfer

3. **NCQ (Native Command Queuing)** (3)
   - NCQ enable
   - NCQ read operations
   - NCQ write operations

4. **Hot-Plug** (3)
   - Hot-plug detection
   - Hot-plug removal
   - Hot-plug stress (100 cycles)

5. **Power Management** (2)
   - Power management enable
   - DEVSLP entry

6. **Error Handling** (2)
   - Timeout handling
   - Error recovery

**Running AHCI Tests**:
```bash
./driver_test --storage
./driver_test --test ahci::ncq_read
```

### Network Driver Tests (e1000)

**Test Cases** (18 total):

1. **Initialization** (3)
   - Device detection
   - MAC address reading
   - Link status detection

2. **Transmission** (3)
   - Send single packet
   - Send large packet
   - Send packet burst

3. **Reception** (2)
   - Receive packet
   - Receive burst

4. **Performance** (1)
   - Throughput test (1 Gbps)

5. **MTU** (2)
   - Default MTU (1500)
   - Jumbo frames (9000)

6. **Link State** (3)
   - Link down event
   - Link up event
   - Link state changes

7. **Mode Control** (2)
   - Promiscuous mode
   - Multicast addresses

8. **Stress Tests** (2)
   - Ping flood (1000 packets)
   - TCP stress test

9. **Error Detection** (1)
   - Packet loss detection

**Running Network Tests**:
```bash
./driver_test --network
./driver_test --test e1000::throughput_1gbps
```

### Graphics Driver Tests (i915)

**Test Cases** (18 total):

1. **Initialization** (2)
   - GPU detection
   - VRAM detection

2. **Mode Setting** (3)
   - 1920x1080@60Hz
   - 3840x2160@60Hz (4K)
   - 144Hz refresh rate

3. **Multi-Monitor** (2)
   - Dual monitor
   - Triple monitor

4. **Page Flipping** (2)
   - Page flip
   - VSync

5. **Framebuffer** (2)
   - Clear operation
   - Pattern drawing

6. **Hardware Cursor** (2)
   - Cursor enable
   - Cursor movement

7. **Hot-Plug** (2)
   - Connect display
   - Disconnect display

8. **Power Management** (2)
   - Power saving mode
   - DPMS off/on

9. **Performance** (1)
   - Frame rate test

**Running Graphics Tests**:
```bash
./driver_test --graphics
./driver_test --test i915::page_flip
```

### Audio Driver Tests (HDA)

**Test Cases** (22 total):

1. **Initialization** (2)
   - Controller detection
   - Codec detection

2. **Playback** (5)
   - Start/stop
   - 44.1 kHz
   - 48 kHz
   - 96 kHz

3. **Recording** (3)
   - Start/stop
   - 48 kHz capture

4. **Formats** (3)
   - 16-bit
   - 24-bit
   - 32-bit

5. **Channels** (2)
   - Stereo
   - 5.1 surround

6. **Multiple Streams** (1)
   - Simultaneous playback and capture

7. **Buffer Handling** (2)
   - Underrun detection
   - Overrun detection

8. **Volume Control** (2)
   - Volume adjustment
   - Mute

9. **Jack Detection** (1)
   - Hot-plug jack detection

10. **Latency** (1)
    - Latency measurement

**Running Audio Tests**:
```bash
./driver_test --audio
./driver_test --test hda::playback_48000
```

### USB Driver Tests (xHCI & HID)

**Test Cases** (17 total):

1. **Initialization** (2)
   - xHCI controller detection
   - Controller initialization

2. **Enumeration** (2)
   - Single device enumeration
   - Multiple devices

3. **Data Transfers** (3)
   - Bulk transfer
   - Interrupt transfer
   - Control transfer

4. **HID Devices** (3)
   - Keyboard
   - Mouse
   - Gamepad

5. **Hot-Plug** (3)
   - Detection
   - Removal
   - Stress test (100 cycles)

6. **USB Speeds** (2)
   - USB 2.0 High-Speed
   - USB 3.0 SuperSpeed

7. **Error Handling** (2)
   - Stall condition
   - Transfer timeout

**Running USB Tests**:
```bash
./driver_test --usb
./driver_test --test usb::hotplug_stress
```

---

## Writing New Tests

### Test Case Structure

```c
static test_result_t test_my_new_test(void) {
    test_log_info("Testing my feature");

    // Test implementation
    TEST_ASSERT_EQUAL(expected, actual);
    TEST_ASSERT_NOT_NULL(pointer);
    TEST_ASSERT(condition);

    return TEST_PASS;
}
```

### Assertion Macros

- `TEST_ASSERT(condition)` - Assert condition is true
- `TEST_ASSERT_EQUAL(expected, actual)` - Assert equality
- `TEST_ASSERT_NOT_EQUAL(a, b)` - Assert inequality
- `TEST_ASSERT_NULL(ptr)` - Assert pointer is NULL
- `TEST_ASSERT_NOT_NULL(ptr)` - Assert pointer is not NULL
- `TEST_ASSERT_STRING_EQUAL(s1, s2)` - Assert string equality
- `TEST_ASSERT_MEMORY_EQUAL(p1, p2, size)` - Assert memory equality

### Registering Tests

```c
void register_my_tests(void) {
    static test_suite_t my_suite = {
        .name = "my_driver",
        .description = "My Driver Tests",
        .setup = my_test_setup,
        .teardown = my_test_teardown,
        .tests = NULL,
        .next = NULL
    };

    static test_case_t test_cases[] = {
        {"test_name", "Test description", test_my_new_test, false, "my_driver"},
    };

    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_case_t); i++) {
        test_register_case(&my_suite, &test_cases[i]);
    }

    test_register_suite(&my_suite);
}
```

### Logging

```c
test_log_info("Informational message");
test_log_debug("Debug message: value=%d", value);
test_log_error(__FILE__, __LINE__, "Error message");
```

### Timing

```c
uint64_t start = test_get_time_us();
// Operation to time
uint64_t elapsed = test_get_time_us() - start;

test_sleep_ms(100);  // Sleep for 100ms
```

### Memory Leak Detection

```c
test_memory_mark();
// Allocate and free memory
if (!test_memory_check_leaks()) {
    // Leak detected
}
```

---

## Mock Device Framework

### Creating Mock Devices

```c
typedef struct {
    test_pci_device_t* pci_dev;
    uint32_t* registers;
    // Device-specific fields
} mock_device_t;

static mock_device_t* create_mock_device(void) {
    mock_device_t* dev = malloc(sizeof(mock_device_t));
    
    // Create PCI device
    dev->pci_dev = test_create_pci_device(vendor_id, device_id);
    
    // Allocate MMIO space
    dev->registers = test_alloc_dma_buffer(size);
    test_pci_set_bar(dev->pci_dev, 0, (uint32_t)(uintptr_t)dev->registers, size);
    
    return dev;
}
```

### Interrupt Simulation

```c
void irq_handler(void* context) {
    // Handle interrupt
}

test_register_irq(IRQ_NUMBER, irq_handler, context);
test_trigger_irq(IRQ_NUMBER);
test_unregister_irq(IRQ_NUMBER);
```

### DMA Buffer Simulation

```c
void* buffer = test_alloc_dma_buffer(4096);
uint64_t phys_addr = test_virt_to_phys(buffer);
// Use buffer
test_free_dma_buffer(buffer);
```

---

## Performance Benchmarking

### Storage Benchmarks

**Sequential Read**:
```bash
./driver_test --test nvme::sequential_bandwidth
```
Target: > 1 GB/s

**Random IOPS**:
```bash
./driver_test --test nvme::random_iops
```
Target: > 100,000 IOPS

### Network Benchmarks

**Throughput**:
```bash
./driver_test --test e1000::throughput_1gbps
```
Target: > 900 Mbps

### Graphics Benchmarks

**Frame Rate**:
```bash
./driver_test --test i915::framerate
```
Target: 60+ FPS at 1080p

### Audio Benchmarks

**Latency**:
```bash
./driver_test --test hda::latency
```
Target: < 50ms

---

## Stress Testing

### 24-Hour Stress Test

To run the full 24-hour stress test, modify `TEST_STRESS_TIME_MS` in the test files and rebuild:

```c
#define TEST_STRESS_TIME_MS 86400000  // 24 hours
```

```bash
make clean && make
./driver_test --stress
```

### Hot-Plug Stress

USB hot-plug stress test (100 cycles):
```bash
./driver_test --test usb::hotplug_stress
```

AHCI hot-plug stress test (100 cycles):
```bash
./driver_test --test ahci::hotplug_stress
```

### Mixed Workload Stress

```bash
./driver_test --test nvme::mixed_workload
```

---

## Hardware Testing

### Test Matrix

See `docs/HARDWARE_COMPATIBILITY.md` for the complete hardware compatibility database with 30+ tested configurations.

### Running on Real Hardware

1. **Build kernel with test support**:
   ```bash
   make CONFIG_DRIVER_TESTS=y
   ```

2. **Boot with test parameter**:
   ```
   kernel.test_drivers=1
   ```

3. **Run tests**:
   ```bash
   /sbin/driver_test --all
   ```

### Hardware-Specific Tests

Some tests require actual hardware:

```c
test_case_t test = {
    .name = "hardware_test",
    .description = "Test requiring real hardware",
    .test_func = test_hardware_specific,
    .requires_hardware = true,  // Skip in mock mode
    .required_driver = "nvme"
};
```

---

## Troubleshooting

### Common Issues

**Issue**: Tests fail with "Memory leak detected"
**Solution**: Check for missing `free()` or `test_free_dma_buffer()` calls

**Issue**: Timeout errors
**Solution**: Increase timeout values or check for deadlocks

**Issue**: Random test failures
**Solution**: Check for race conditions, use proper synchronization

### Debug Mode

Enable verbose logging:
```bash
./driver_test --all --verbose
```

### Memory Debugging

Check for leaks after each test:
```c
test_memory_mark();
// Test code
TEST_ASSERT(test_memory_check_leaks());
```

### Viewing Test Results

Test output includes:
- Test name and description
- Pass/Fail status
- Error messages with file and line number
- Execution time
- Summary statistics

Example output:
```
[INFO] Running suite: nvme - NVMe Storage Driver Tests
  [TEST] device_detection
    PASS
  [TEST] sequential_read
    PASS

========================================
Test Results:
  Total:   18
  Passed:  18 (100.0%)
  Failed:  0 (0.0%)
  Skipped: 0
  Errors:  0
  Time:    2.45 seconds
========================================
```

---

## Continuous Integration

### Automated Testing

Run tests in CI pipeline:
```bash
#!/bin/bash
cd tests/drivers
make clean && make || exit 1
./driver_test --all --quick || exit 1
```

### Test Reports

Generate test report:
```bash
./driver_test --all > test_results.log 2>&1
```

### Performance Regression Detection

Compare benchmark results against baseline:
```bash
./run_benchmarks.sh > current.txt
diff baseline.txt current.txt
```

---

## Next Steps

1. **Run All Tests**: `./driver_test --all`
2. **Review Results**: Check for failures
3. **Run Stress Tests**: `./driver_test --stress`
4. **Test on Hardware**: See Hardware Testing section
5. **Add New Tests**: See Writing New Tests section

For hardware compatibility information, see `docs/HARDWARE_COMPATIBILITY.md`.

For driver implementation details, see individual driver source files in `kernel/drivers/`.
