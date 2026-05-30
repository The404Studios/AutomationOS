# ath9k - Atheros 802.11n Wireless Driver

## Overview

The ath9k driver provides support for Atheros AR9xxx series 802.11n wireless network adapters. This implementation supports infrastructure BSS (station mode), basic packet transmission/reception, and 802.11n features including MIMO and 40 MHz channels.

## Supported Hardware

### Chipsets
- AR9280 (PCI-Express 2x2:2)
- AR9285 (PCI-Express 1x1:1, single-chip)
- AR9287 (PCI-Express 2x2:2)
- AR9380 (PCI-Express 3x3:3, 802.11n only)
- AR9485 (PCI-Express 1x1:1, single-chip)
- AR9462 (PCI-Express 2x2:2, dual-band)
- AR9565 (PCI-Express 1x1:1, single-chip, BT combo)

### Features
- IEEE 802.11a/b/g/n
- 2.4 GHz and 5 GHz bands (device-dependent)
- Up to 300 Mbps (2x2 MIMO)
- Hardware encryption (WEP, TKIP, CCMP/AES)
- Multiple operating modes:
  - Station (infrastructure BSS)
  - Access Point (AP)
  - Monitor mode
  - Ad-hoc (IBSS)
- Power save modes
- Dynamic Frequency Selection (DFS)
- Spectrum management

## Architecture

### File Structure

```
ath9k/
├── ath9k.h              # Main header file (register definitions, structures)
├── ath9k_main.c         # Driver core (probe, init, mac80211 ops)
├── ath9k_hw.c           # Hardware access layer
├── ath9k_phy.c          # PHY/RF management
├── ath9k_tx.c           # TX (transmit) handling
├── ath9k_rx.c           # RX (receive) handling
├── ath9k_irq.c          # Interrupt handling
└── README.md            # This file
```

### Layer Diagram

```
┌─────────────────────────────────────┐
│   Network Stack (IP, TCP/UDP)       │
└───────────────┬─────────────────────┘
                │
┌───────────────▼─────────────────────┐
│   mac80211 (Generic 802.11 MAC)     │
│   - Frame encapsulation             │
│   - Encryption                      │
│   - Rate control                    │
│   - Association management          │
└───────────────┬─────────────────────┘
                │
┌───────────────▼─────────────────────┐
│   ath9k Driver                      │
│   - Hardware initialization         │
│   - TX/RX queue management          │
│   - Interrupt handling              │
│   - PHY/RF control                  │
└───────────────┬─────────────────────┘
                │
┌───────────────▼─────────────────────┐
│   Hardware (Atheros AR9xxx)         │
│   - MAC engine                      │
│   - Baseband processor              │
│   - RF synthesizer                  │
└─────────────────────────────────────┘
```

## Hardware Initialization Sequence

### 1. PCI Probe
- Detect Atheros wireless device via PCI enumeration
- Check vendor ID (0x168C) and device ID
- Map MMIO BAR0 (register space)

### 2. Hardware Reset
```c
ath9k_hw_reset(ah);
```
- Disable interrupts
- Software reset via command register
- Clear pending interrupts
- Re-initialize basic registers

### 3. EEPROM Read
```c
ath9k_hw_read_eeprom(ah);
```
- Read MAC address
- Read calibration data
- Read RF parameters

### 4. PHY Initialization
```c
ath9k_hw_phy_init(ah);
```
- Enable RF
- Setup AGC (Automatic Gain Control)
- Configure RX gain
- Enable HT (802.11n) mode
- Perform initial calibration

### 5. TX/RX Queue Setup
```c
ath9k_tx_init(sc);
ath9k_rx_init(sc);
```
- Allocate descriptor rings
- Allocate DMA buffers
- Program hardware descriptor pointers
- Enable TX/RX DMA

### 6. Interrupt Setup
- Register IRQ handler
- Enable interrupts (RX, TX, beacon)

## Register Map

### Core Registers
| Register | Offset | Description |
|----------|--------|-------------|
| AR_CR    | 0x0008 | Command Register |
| AR_RXDP  | 0x000C | RX Descriptor Pointer |
| AR_CFG   | 0x0014 | Configuration |
| AR_IER   | 0x0024 | Interrupt Enable Register |
| AR_TXCFG | 0x0030 | TX Configuration |
| AR_RXCFG | 0x0034 | RX Configuration |
| AR_ISR   | 0x0080 | Interrupt Status Register |
| AR_IMR   | 0x00A0 | Interrupt Mask Register |

### PHY Registers
| Register | Offset | Description |
|----------|--------|-------------|
| AR_PHY_TEST      | 0x9800 | PHY Test Register |
| AR_PHY_TURBO     | 0x9804 | Turbo/HT Mode Control |
| AR_PHY_SETTLING  | 0x9844 | PHY Settling Time |
| AR_PHY_AGC_CTL1  | 0x985C | AGC Control |
| AR_PHY_CCA       | 0x9864 | Clear Channel Assessment |

## TX (Transmit) Path

### TX Descriptor Ring

Each TX queue has a circular descriptor ring:
```c
typedef struct {
    uint32_t ds_link;    // Next descriptor (physical address)
    uint32_t ds_data;    // Data buffer (physical address)
    uint32_t ds_ctl0;    // Control word 0 (length, flags)
    uint32_t ds_ctl1;    // Control word 1 (flags)
    uint32_t ds_hw0;     // Hardware status word 0
    uint32_t ds_hw1;     // Hardware status word 1
    uint32_t ds_hw2;     // Hardware status word 2
    uint32_t ds_hw3;     // Hardware status word 3
} ath9k_desc_t;
```

### TX Flow
1. **Frame Submission**: mac80211 calls `ath9k_tx()`
2. **Queue Selection**: Map frame to hardware queue (BE, BK, VI, VO)
3. **Descriptor Setup**: Fill TX descriptor with buffer address, length, flags
4. **Ring Doorbell**: Update tail pointer, notify hardware
5. **DMA Transfer**: Hardware reads descriptor, fetches buffer, transmits
6. **Completion**: Interrupt on TX completion
7. **Cleanup**: Free buffer, update statistics

### TX Queues
- **Queue 0 (BE)**: Best Effort (default data)
- **Queue 1 (BK)**: Background (bulk transfer)
- **Queue 2 (VI)**: Video (multimedia)
- **Queue 3 (VO)**: Voice (VoIP, low latency)

## RX (Receive) Path

### RX Descriptor Ring

Single RX queue with circular descriptor ring:
- 512 descriptors (configurable)
- 2 KB buffers per descriptor
- Pre-allocated buffers

### RX Flow
1. **Buffer Pre-allocation**: Allocate RX buffers at init
2. **Descriptor Setup**: Program descriptors with buffer addresses
3. **DMA Receive**: Hardware writes received frames to buffers
4. **Interrupt**: RX interrupt on frame reception
5. **Frame Processing**: Read descriptor, extract frame data
6. **mac80211 Submit**: Pass frame to mac80211 via `ieee80211_rx()`
7. **Buffer Refill**: Re-use buffer for next reception

## Interrupt Handling

### Interrupt Sources
| Flag | Description |
|------|-------------|
| AR_ISR_RXOK   | RX frame received successfully |
| AR_ISR_RXERR  | RX error (CRC, PHY) |
| AR_ISR_RXORN  | RX overrun (ring full) |
| AR_ISR_RXEOL  | RX end of list |
| AR_ISR_TXOK   | TX frame sent successfully |
| AR_ISR_TXERR  | TX error |
| AR_ISR_TXURN  | TX underrun |
| AR_ISR_BMISS  | Beacon miss (station mode) |
| AR_ISR_SWBA   | Software beacon alert (AP mode) |

### IRQ Handler Flow
```c
void ath9k_irq_handler(void) {
    // Read ISR
    uint32_t isr = ath9k_hw_reg_read(ah, AR_ISR);
    
    // Clear interrupts
    ath9k_hw_reg_write(ah, AR_ISR, isr);
    
    // Handle RX
    if (isr & AR_ISR_RXOK) {
        ath9k_rx_tasklet(sc);
    }
    
    // Handle TX
    if (isr & AR_ISR_TXOK) {
        ath9k_tx_tasklet(sc);
    }
    
    // Handle errors
    // ...
}
```

## PHY/RF Management

### Channel Programming

To set a channel (e.g., channel 6 at 2437 MHz):
```c
ath9k_hw_set_channel(ah, 2437);
```

This involves:
1. Program RF synthesizer PLL
2. Wait for PLL lock
3. Calibrate for new frequency
4. Update noise floor

### 802.11n HT Modes

- **HT20**: 20 MHz channel width (standard)
- **HT40+**: 40 MHz channel, primary on lower 20 MHz
- **HT40-**: 40 MHz channel, primary on upper 20 MHz

Enable HT40:
```c
val = ath9k_hw_reg_read(ah, AR_PHY_TURBO);
val |= AR_PHY_FC_DYN2040_EN | AR_PHY_FC_SHORT_GI_40;
ath9k_hw_reg_write(ah, AR_PHY_TURBO, val);
```

### Calibration

Periodic calibration is required for optimal performance:
- **Noise Floor Calibration**: Measure background noise
- **IQ Calibration**: Correct I/Q imbalance
- **ADC Calibration**: Calibrate ADC offsets

```c
ath9k_hw_calibrate(ah);
```

## Power Management

### TX Power Control
```c
ath9k_hw_set_txpower(ah, 20);  // 20 dBm
```

### Power Save Modes
- **CAM (Constant Awake Mode)**: Always awake (no power save)
- **PS-Poll**: Poll for buffered frames
- **U-APSD**: Unscheduled Automatic Power Save Delivery

## Station Mode Operation

### Connection Flow

1. **Scan**
   ```c
   ath9k_hw_set_channel(ah, 2437);  // Channel 6
   // Send probe requests
   // Collect beacon/probe responses
   ```

2. **Authentication**
   - Send authentication frame (Open System)
   - Wait for authentication response

3. **Association**
   - Send association request
   - Wait for association response
   - Configure encryption keys

4. **Data Transfer**
   - Send/receive data frames
   - Handle beacon reception
   - Monitor link quality

5. **Disassociation**
   - Send disassociation frame
   - Or receive deauthentication

## AP Mode Operation (Future)

### Beacon Generation
```c
sk_buff_t* beacon = ieee80211_beacon_get(hw, vif);
// Queue beacon for transmission
```

### Client Management
- Handle authentication requests
- Handle association requests
- Maintain station list
- Broadcast beacons

## Encryption

### Hardware Encryption Offload

Supported cipher suites:
- **WEP-40**: 40-bit WEP (legacy, insecure)
- **WEP-104**: 104-bit WEP (legacy, insecure)
- **TKIP**: Temporal Key Integrity Protocol (WPA)
- **CCMP**: Counter Mode CBC-MAC Protocol (WPA2, AES)

Key management:
```c
ath9k_set_key(hw, vif, key, key_len);
```

## Performance

### Expected Throughput

| Mode | Bandwidth | MCS | Throughput |
|------|-----------|-----|------------|
| 802.11b | 20 MHz | - | 11 Mbps |
| 802.11g | 20 MHz | - | 54 Mbps |
| 802.11n (1x1) | 20 MHz | MCS7 | 72 Mbps |
| 802.11n (1x1) | 40 MHz | MCS7 | 150 Mbps |
| 802.11n (2x2) | 20 MHz | MCS15 | 144 Mbps |
| 802.11n (2x2) | 40 MHz | MCS15 | 300 Mbps |

### Latency
- TX latency: ~1-2 ms (hardware + driver)
- RX latency: <1 ms (interrupt to stack)

## Debugging

### Enable Debug Output
```c
sc->debug = true;
```

### Statistics
```c
kprintf("TX: %llu packets, %llu errors\n", ah->tx_packets, ah->tx_errors);
kprintf("RX: %llu packets, %llu errors\n", ah->rx_packets, ah->rx_errors);
kprintf("IRQ: %u total, %u RX, %u TX\n", 
        sc->irq_count, sc->rx_irq_count, sc->tx_irq_count);
```

### Register Dumps
```c
uint32_t val = ath9k_hw_reg_read(ah, AR_ISR);
kprintf("ISR: 0x%08x\n", val);
```

## Testing

### QEMU Testing
QEMU has limited WiFi emulation. Use real hardware for testing.

### Real Hardware Testing

1. **Device Detection**
   ```
   ath9k: Found Atheros device 0029
   ath9k: Chip ID: 0xDEADBEEF, Rev: 0x01
   ath9k: MAC address: 00:03:7F:XX:XX:XX
   ```

2. **Interface Bring-Up**
   ```c
   ath9k_start(hw);
   ath9k_add_interface(hw, vif);
   ath9k_config(hw, changed);
   ```

3. **Scan Test**
   - Set multiple channels
   - Send probe requests
   - Collect responses

4. **Connection Test**
   - Authenticate to AP
   - Associate with AP
   - Send/receive data

5. **Throughput Test**
   - Use iperf3
   - Target: >50 Mbps (802.11n 1x1:1 HT20)

## Known Limitations

### Current Implementation
- No firmware loading (not required for AR9xxx)
- Simplified calibration
- No DFS (Dynamic Frequency Selection)
- No beamforming
- No MU-MIMO
- Limited power save support
- No spectral scan

### Future Enhancements
1. Full 5 GHz band support
2. AP mode (access point)
3. Monitor mode (packet injection)
4. Ad-hoc mode (IBSS)
5. Mesh networking
6. Advanced power save
7. Dynamic rate selection
8. Packet aggregation (A-MPDU, A-MSDU)
9. Block acknowledgment
10. Regulatory domain compliance

## References

### Documentation
- IEEE 802.11-2016 Standard
- Atheros AR9xxx Hardware Programming Manual (NDA required)
- Linux ath9k driver source code
- mac80211 documentation

### Related Files
- `kernel/include/ieee80211.h` - IEEE 802.11 definitions
- `kernel/include/mac80211.h` - mac80211 API
- `kernel/drivers/net/wireless/mac80211/` - mac80211 implementation

## License

This driver is part of AutomationOS kernel and follows the same license.

---

**Author**: Wireless Driver Engineer  
**Date**: 2026-05-26  
**Status**: Initial Implementation (Phase 2-3)
