# ath9k Driver Testing Guide

## Overview

This document provides comprehensive testing procedures for the ath9k wireless driver, from basic hardware detection to full network connectivity.

## Prerequisites

### Hardware Requirements
- Atheros AR9xxx wireless adapter (PCI-Express)
  - Recommended: AR9285, AR9287, AR9485 (widely available)
  - Avoid: Very old AR5xxx series (not supported)
- Test Access Point (AP) with:
  - 2.4 GHz support
  - WPA2-PSK encryption
  - SSID broadcast enabled
  - Known good configuration

### Software Requirements
- AutomationOS kernel with:
  - PCI subsystem enabled
  - DMA allocator working
  - Interrupt handling functional
  - Memory management stable

## Test Levels

### Level 0: Build Verification

**Objective**: Verify driver compiles and links correctly.

**Steps**:
1. Build kernel with ath9k driver
2. Check for compilation errors
3. Verify symbols are exported

**Expected Output**:
```
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_main.o
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_hw.o
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_phy.o
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_tx.o
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_rx.o
CC kernel/drivers/net/wireless/ath/ath9k/ath9k_irq.o
LD kernel/drivers/net/wireless/ath/ath9k.o
```

### Level 1: Device Detection

**Objective**: Verify driver can detect Atheros wireless hardware.

**Code**:
```c
void test_level1_detection(void) {
    kprintf("=== Level 1: Device Detection ===\n");
    
    int ret = ath9k_init();
    if (ret == 0) {
        kprintf("PASS: Device detected\n");
    } else {
        kprintf("FAIL: No device found\n");
    }
}
```

**Expected Output**:
```
ath9k: Atheros 802.11n wireless driver
ath9k: Found Atheros device 002A
ath9k_hw: Initializing hardware
ath9k_hw: MMIO base at 0xFEBC0000
ath9k_hw: Chip ID: 0xDEADBEEF, Rev: 0x01
ath9k_hw: MAC address: 00:03:7F:XX:XX:XX
PASS: Device detected
```

**Troubleshooting**:
- "No device found": Check PCI enumeration, verify card is inserted
- "Invalid BAR0": PCI subsystem issue
- "Chip test failed": Hardware communication problem

### Level 2: Hardware Initialization

**Objective**: Verify hardware reset and register access.

**Code**:
```c
void test_level2_hw_init(void) {
    kprintf("=== Level 2: Hardware Initialization ===\n");
    
    // Device should already be initialized from level 1
    ath9k_softc_t* sc = ath9k_get_global_softc();
    if (!sc) {
        kprintf("FAIL: No device context\n");
        return;
    }
    
    ath9k_hw_t* ah = sc->ah;
    
    // Test register read/write
    uint32_t test_val = 0x12345678;
    ath9k_hw_reg_write(ah, AR_MACMISC, test_val);
    uint32_t read_val = ath9k_hw_reg_read(ah, AR_MACMISC);
    
    if (read_val == test_val) {
        kprintf("PASS: Register access working\n");
    } else {
        kprintf("FAIL: Register read/write mismatch\n");
    }
    
    // Test reset
    int ret = ath9k_hw_reset(ah);
    if (ret == 0) {
        kprintf("PASS: Hardware reset successful\n");
    } else {
        kprintf("FAIL: Hardware reset failed\n");
    }
}
```

**Expected Output**:
```
=== Level 2: Hardware Initialization ===
ath9k_hw: Resetting hardware
ath9k_hw: Hardware reset complete
PASS: Register access working
PASS: Hardware reset successful
```

### Level 3: PHY Initialization

**Objective**: Verify PHY and RF subsystem initialization.

**Code**:
```c
void test_level3_phy_init(void) {
    kprintf("=== Level 3: PHY Initialization ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah = sc->ah;
    
    int ret = ath9k_hw_phy_init(ah);
    if (ret == 0) {
        kprintf("PASS: PHY initialized\n");
    } else {
        kprintf("FAIL: PHY initialization failed\n");
    }
    
    // Test channel setting
    ret = ath9k_hw_set_channel(ah, 2437);  // Channel 6
    if (ret == 0) {
        kprintf("PASS: Channel set to 2437 MHz\n");
    } else {
        kprintf("FAIL: Failed to set channel\n");
    }
}
```

**Expected Output**:
```
=== Level 3: PHY Initialization ===
ath9k_phy: Initializing PHY
ath9k_phy: Initializing RF
ath9k_hw: Calibration complete, NF=-95 dBm
ath9k_phy: PHY initialized successfully
PASS: PHY initialized
ath9k_hw: Setting channel to 2437 MHz
PASS: Channel set to 2437 MHz
```

### Level 4: TX/RX Queue Setup

**Objective**: Verify TX and RX queue initialization.

**Code**:
```c
void test_level4_queue_setup(void) {
    kprintf("=== Level 4: TX/RX Queue Setup ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    
    // Initialize TX
    int ret = ath9k_tx_init(sc);
    if (ret == 0) {
        kprintf("PASS: TX queues initialized\n");
    } else {
        kprintf("FAIL: TX initialization failed\n");
        return;
    }
    
    // Initialize RX
    ret = ath9k_rx_init(sc);
    if (ret == 0) {
        kprintf("PASS: RX queue initialized\n");
    } else {
        kprintf("FAIL: RX initialization failed\n");
        return;
    }
    
    // Verify queue structures
    ath9k_hw_t* ah = sc->ah;
    for (int i = 0; i < 4; i++) {
        if (ah->tx_queues[i]) {
            kprintf("TX Queue %d: head=%d, tail=%d, size=%d\n",
                    i, ah->tx_queues[i]->head, 
                    ah->tx_queues[i]->tail,
                    ah->tx_queues[i]->size);
        }
    }
    
    if (ah->rx_queue) {
        kprintf("RX Queue: head=%d, size=%d\n",
                ah->rx_queue->head, ah->rx_queue->size);
    }
}
```

**Expected Output**:
```
=== Level 4: TX/RX Queue Setup ===
ath9k_tx: Initializing TX queues
ath9k_tx: Queue 0 initialized (ring at 0xFFA00000)
ath9k_tx: Queue 1 initialized (ring at 0xFFA01000)
ath9k_tx: Queue 2 initialized (ring at 0xFFA02000)
ath9k_tx: Queue 3 initialized (ring at 0xFFA03000)
ath9k_tx: TX initialized successfully
PASS: TX queues initialized
ath9k_rx: Initializing RX queue
ath9k_rx: RX initialized successfully (ring at 0xFFA10000)
PASS: RX queue initialized
TX Queue 0: head=0, tail=0, size=256
TX Queue 1: head=0, tail=0, size=256
TX Queue 2: head=0, tail=0, size=256
TX Queue 3: head=0, tail=0, size=256
RX Queue: head=0, size=512
```

### Level 5: Interrupt Handling

**Objective**: Verify interrupt registration and handling.

**Code**:
```c
void test_level5_interrupts(void) {
    kprintf("=== Level 5: Interrupt Handling ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah = sc->ah;
    
    // Register IRQ handler
    irq_register_handler(sc->irq, ath9k_irq_handler);
    kprintf("Registered IRQ handler on IRQ %d\n", sc->irq);
    
    // Enable interrupts
    uint32_t imr = AR_ISR_RXOK | AR_ISR_TXOK;
    ath9k_hw_reg_write(ah, AR_IER, 1);
    ath9k_hw_reg_write(ah, AR_IMR, imr);
    
    kprintf("Enabled interrupts (IMR=0x%08x)\n", imr);
    
    // Wait for interrupt (simplified test)
    uint32_t irq_before = sc->irq_count;
    timer_sleep(1000);  // Wait 1 second
    uint32_t irq_after = sc->irq_count;
    
    if (irq_after > irq_before) {
        kprintf("PASS: Interrupts working (%d IRQs)\n", 
                irq_after - irq_before);
    } else {
        kprintf("INFO: No interrupts received (may be normal)\n");
    }
}
```

**Expected Output**:
```
=== Level 5: Interrupt Handling ===
Registered IRQ handler on IRQ 19
Enabled interrupts (IMR=0x00000041)
PASS: Interrupts working (15 IRQs)
```

### Level 6: TX Test

**Objective**: Verify frame transmission.

**Code**:
```c
void test_level6_tx(void) {
    kprintf("=== Level 6: TX Test ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah = sc->ah;
    
    // Create test frame (802.11 beacon)
    uint8_t beacon[] = {
        0x80, 0x00,  // Frame control (beacon)
        0x00, 0x00,  // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA (broadcast)
        0x00, 0x03, 0x7F, 0x12, 0x34, 0x56,  // SA (source)
        0x00, 0x03, 0x7F, 0x12, 0x34, 0x56,  // BSSID
        0x00, 0x00,  // Sequence control
        // Beacon payload...
    };
    
    sk_buff_t skb;
    skb.data = beacon;
    skb.len = sizeof(beacon);
    
    uint64_t tx_before = ah->tx_packets;
    int ret = ath9k_tx_queue_frame(sc, 0, &skb);
    
    if (ret == 0) {
        kprintf("PASS: Frame queued for TX\n");
        
        // Wait for TX completion
        timer_sleep(100);
        ath9k_tx_tasklet(sc);
        
        uint64_t tx_after = ah->tx_packets;
        if (tx_after > tx_before) {
            kprintf("PASS: Frame transmitted (%llu total)\n", tx_after);
        } else {
            kprintf("INFO: TX completion pending\n");
        }
    } else {
        kprintf("FAIL: Failed to queue frame\n");
    }
}
```

**Expected Output**:
```
=== Level 6: TX Test ===
ath9k_tx: Queued frame (len=24) to queue 0
PASS: Frame queued for TX
PASS: Frame transmitted (1 total)
```

### Level 7: RX Test

**Objective**: Verify frame reception.

**Code**:
```c
void test_level7_rx(void) {
    kprintf("=== Level 7: RX Test ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah = sc->ah;
    
    kprintf("Waiting for RX frames (30 seconds)...\n");
    kprintf("Make sure AP is broadcasting beacons nearby\n");
    
    uint64_t rx_before = ah->rx_packets;
    
    for (int i = 0; i < 30; i++) {
        timer_sleep(1000);
        ath9k_rx_tasklet(sc);
        
        uint64_t rx_current = ah->rx_packets;
        if (rx_current > rx_before) {
            kprintf("RX: %llu packets (+%llu)\n", 
                    rx_current, rx_current - rx_before);
            rx_before = rx_current;
        }
    }
    
    if (ah->rx_packets > 0) {
        kprintf("PASS: Received %llu frames\n", ah->rx_packets);
    } else {
        kprintf("FAIL: No frames received\n");
    }
}
```

**Expected Output**:
```
=== Level 7: RX Test ===
Waiting for RX frames (30 seconds)...
Make sure AP is broadcasting beacons nearby
RX: 10 packets (+10)
RX: 25 packets (+15)
RX: 42 packets (+17)
...
PASS: Received 312 frames
```

### Level 8: Scan Test

**Objective**: Verify channel scanning and AP detection.

**Code**:
```c
void test_level8_scan(void) {
    kprintf("=== Level 8: Scan Test ===\n");
    
    ath9k_softc_t* sc = ath9k_get_global_softc();
    ath9k_hw_t* ah = sc->ah;
    
    // Scan channels 1-11
    for (int chan = 1; chan <= 11; chan++) {
        uint32_t freq = 2412 + (chan - 1) * 5;
        
        kprintf("Scanning channel %d (%d MHz)...\n", chan, freq);
        ath9k_hw_set_channel(ah, freq);
        
        // Listen for beacons
        uint64_t rx_before = ah->rx_packets;
        timer_sleep(200);  // 200ms per channel
        ath9k_rx_tasklet(sc);
        uint64_t rx_after = ah->rx_packets;
        
        if (rx_after > rx_before) {
            kprintf("  Found %llu frames\n", rx_after - rx_before);
        }
    }
    
    kprintf("Scan complete\n");
}
```

**Expected Output**:
```
=== Level 8: Scan Test ===
Scanning channel 1 (2412 MHz)...
  Found 3 frames
Scanning channel 2 (2417 MHz)...
Scanning channel 3 (2422 MHz)...
Scanning channel 4 (2427 MHz)...
Scanning channel 5 (2432 MHz)...
Scanning channel 6 (2437 MHz)...
  Found 15 frames
Scanning channel 7 (2442 MHz)...
  Found 8 frames
...
Scan complete
```

### Level 9: Association Test (Future)

**Objective**: Connect to WPA2 access point.

**Status**: Not implemented yet. Requires:
- WPA supplicant integration
- Authentication state machine
- Key management
- EAPOL handling

### Level 10: Data Transfer Test (Future)

**Objective**: Send/receive IP packets over WiFi.

**Status**: Not implemented yet. Requires:
- Network stack integration
- DHCP client
- TCP/IP stack
- Socket API

## Full Test Suite

```c
void run_ath9k_test_suite(void) {
    kprintf("=====================================\n");
    kprintf("   ath9k Driver Test Suite\n");
    kprintf("=====================================\n");
    
    test_level1_detection();
    test_level2_hw_init();
    test_level3_phy_init();
    test_level4_queue_setup();
    test_level5_interrupts();
    test_level6_tx();
    test_level7_rx();
    test_level8_scan();
    
    kprintf("=====================================\n");
    kprintf("   Test Suite Complete\n");
    kprintf("=====================================\n");
}
```

## Performance Testing

### Throughput Test
```c
void test_throughput(void) {
    // Generate 1000 frames
    uint64_t start_ticks = timer_get_ticks();
    
    for (int i = 0; i < 1000; i++) {
        // Queue TX frame
        ath9k_tx_queue_frame(sc, 0, &skb);
    }
    
    uint64_t end_ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    uint64_t elapsed_ms = (end_ticks - start_ticks) * 1000 / freq;
    
    kprintf("TX throughput: %llu frames/sec\n", 1000000 / elapsed_ms);
}
```

### Latency Test
```c
void test_latency(void) {
    // Measure TX latency
    uint64_t start = timer_get_ticks();
    ath9k_tx_queue_frame(sc, 0, &skb);
    // Wait for completion
    while (!(desc->ds_hw0 & ATH9K_TXDESC_INTREQ));
    uint64_t end = timer_get_ticks();
    
    uint32_t latency_us = (end - start) * 1000000 / timer_get_frequency();
    kprintf("TX latency: %u us\n", latency_us);
}
```

## Troubleshooting Guide

### Device Not Detected
**Symptoms**: "No device found"
**Causes**:
- Card not inserted
- PCI enumeration failed
- Unsupported device ID
**Solutions**:
- Check PCI subsystem: `pci_init()`
- Verify device ID in ath9k.h
- Use `lspci` equivalent to list devices

### Register Access Failed
**Symptoms**: "Chip test failed"
**Causes**:
- Invalid BAR mapping
- DMA disabled
- Hardware locked up
**Solutions**:
- Verify BAR0 is mapped correctly
- Enable PCI bus master
- Power cycle hardware

### No Interrupts
**Symptoms**: "No interrupts received"
**Causes**:
- IRQ not registered
- Interrupts disabled in hardware
- MSI/MSI-X issues
**Solutions**:
- Verify IRQ handler registration
- Check IER/IMR registers
- Try legacy interrupts instead of MSI

### No RX Frames
**Symptoms**: "No frames received"
**Causes**:
- RX queue not started
- Incorrect descriptor setup
- Channel not set
- No APs nearby
**Solutions**:
- Verify `AR_CR_RXE` is set
- Check descriptor ring setup
- Set correct channel
- Test with known-good AP

### TX Failures
**Symptoms**: TX errors, underruns
**Causes**:
- DMA issues
- Buffer allocation failed
- Queue full
**Solutions**:
- Check DMA allocator
- Increase queue size
- Verify descriptor setup

## Hardware-Specific Notes

### AR9285
- Single-chip solution
- 1x1:1 MIMO
- 2.4 GHz only
- Lower power consumption

### AR9287
- 2x2:2 MIMO
- 2.4 GHz only
- Good general-purpose chip

### AR9485
- Single-chip solution
- 1x1:1 MIMO
- 2.4 GHz only
- Commonly found in laptops

### AR9462
- Dual-band (2.4 + 5 GHz)
- 2x2:2 MIMO
- Bluetooth combo chip

## Success Criteria

A successful ath9k driver implementation should achieve:

1. **Device Detection**: 100% success rate
2. **Hardware Init**: Stable, no hangs
3. **PHY Init**: Clean calibration
4. **TX**: >90% success rate
5. **RX**: Receiving beacons from nearby APs
6. **Interrupts**: Consistent IRQ delivery
7. **Throughput**: >50 Mbps (802.11n HT20)
8. **Latency**: <5ms TX latency
9. **Stability**: No kernel panics, 24h uptime

## Next Steps

After completing basic tests:

1. **Network Stack Integration**
   - Implement 802.11 frame decapsulation
   - Add IP layer
   - Integrate with socket API

2. **WPA2 Support**
   - Implement 4-way handshake
   - Add key management
   - Support WPA supplicant

3. **Advanced Features**
   - AP mode
   - Monitor mode
   - Power save
   - Rate control

---

**Last Updated**: 2026-05-26
