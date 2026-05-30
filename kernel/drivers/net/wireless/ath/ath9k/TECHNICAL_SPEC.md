# ath9k Driver Technical Specification

## 1. Hardware Architecture

### 1.1 Atheros AR9xxx Chipset Overview

The Atheros AR9xxx family consists of single-chip 802.11n WLAN solutions with integrated:
- MAC (Media Access Control) engine
- Baseband processor
- RF (Radio Frequency) transceiver
- PCIe interface

**Block Diagram**:
```
┌─────────────────────────────────────────────────┐
│              AR9xxx Chipset                     │
│                                                 │
│  ┌────────────┐     ┌──────────────┐          │
│  │  PCIe I/F  │────▶│ DMA Engine   │          │
│  └────────────┘     └──────┬───────┘          │
│                            │                    │
│  ┌────────────┐     ┌──────▼───────┐          │
│  │   MMIO     │◀───▶│  MAC Engine  │          │
│  │  Registers │     └──────┬───────┘          │
│  └────────────┘            │                    │
│                     ┌──────▼───────┐          │
│                     │  Baseband    │          │
│                     │  Processor   │          │
│                     └──────┬───────┘          │
│                            │                    │
│                     ┌──────▼───────┐          │
│                     │  RF          │◀─────▶ Antenna
│                     │  Transceiver │          │
│                     └──────────────┘          │
└─────────────────────────────────────────────────┘
```

### 1.2 Register Space

The AR9xxx exposes a 64KB MMIO register space via BAR0:

| Address Range | Description |
|---------------|-------------|
| 0x0000-0x0FFF | MAC registers |
| 0x1000-0x1FFF | QCU (Queue Control Unit) registers |
| 0x2000-0x2FFF | DCU (Distributed Coordination Function) registers |
| 0x3000-0x3FFF | DMA engine registers |
| 0x4000-0x7FFF | Reserved |
| 0x8000-0x8FFF | PCIe interface registers |
| 0x9800-0x9FFF | PHY/Baseband registers |
| 0xA000-0xBFFF | Radio (RF) registers |
| 0xC000-0xFFFF | Reserved |

### 1.3 DMA Architecture

**TX Path**:
```
Host Memory                    Card
┌──────────────┐              ┌──────────────┐
│ TX Descriptor│──────────────▶│  DMA Read    │
│   Ring       │              │  Engine      │
└──────────────┘              └──────┬───────┘
                                     │
┌──────────────┐              ┌─────▼────────┐
│  TX Buffer   │──────────────▶│  FIFO        │
│  (Frame)     │              │  Buffer      │
└──────────────┘              └──────┬───────┘
                                     │
                              ┌──────▼───────┐
                              │  MAC Engine  │
                              └──────┬───────┘
                                     │
                              ┌──────▼───────┐
                              │  Baseband    │
                              └──────┬───────┘
                                     │
                                  RF Out
```

**RX Path**:
```
Card                          Host Memory
┌──────────────┐              ┌──────────────┐
│  RF In       │              │  RX Buffer   │
└──────┬───────┘              │  (Frame)     │
       │                      └──────▲───────┘
┌──────▼───────┐                     │
│  Baseband    │              ┌──────┴───────┐
└──────┬───────┘              │  DMA Write   │
       │                      │  Engine      │
┌──────▼───────┐              └──────▲───────┘
│  MAC Engine  │                     │
└──────┬───────┘              ┌──────┴───────┐
       │                      │ RX Descriptor│
┌──────▼───────┐              │   Ring       │
│  FIFO        │──────────────▶└──────────────┘
│  Buffer      │
└──────────────┘
```

## 2. Descriptor Format

### 2.1 TX Descriptor

```c
struct ath9k_tx_desc {
    uint32_t ds_link;       // [31:2] Next descriptor physical address
                            // [1:0]  Reserved
    
    uint32_t ds_data;       // [31:2] Data buffer physical address
                            // [1:0]  Reserved
    
    uint32_t ds_ctl0;       // Control word 0
                            // [31:30] Reserved
                            // [29:16] Frame length
                            // [15:0]  TX power, rate, etc.
    
    uint32_t ds_ctl1;       // Control word 1
                            // [31]    VEOL (Virtual End Of List)
                            // [30]    Clear DEST_MASK
                            // [29:28] Reserved
                            // [27:24] Antenna select
                            // [23:16] TX rate
                            // [15:0]  Flags
    
    uint32_t ds_hw0;        // Hardware status 0
                            // [31]    Done
                            // [30:16] TX sequence number
                            // [15:0]  Status flags
    
    uint32_t ds_hw1;        // Hardware status 1
                            // [31:16] TX timestamp (lower)
                            // [15:0]  Reserved
    
    uint32_t ds_hw2;        // Hardware status 2
                            // [31:0]  TX timestamp (upper)
    
    uint32_t ds_hw3;        // Hardware status 3
                            // [31:16] Final TX rate
                            // [15:0]  BA bitmap
};
```

**Control Flags (ds_ctl1)**:
- Bit 31: VEOL - Virtual End Of List
- Bit 4: INTREQ - Request interrupt on completion
- Bit 3: CTSENA - Enable CTS protection
- Bit 2: RTSENA - Enable RTS/CTS
- Bit 1: NOACK - No ACK required
- Bit 0: CLRDMASK - Clear destination mask

### 2.2 RX Descriptor

```c
struct ath9k_rx_desc {
    uint32_t ds_link;       // [31:2] Next descriptor physical address
                            // [1:0]  Reserved
    
    uint32_t ds_data;       // [31:2] Data buffer physical address
                            // [1:0]  Reserved
    
    uint32_t ds_ctl0;       // Control word 0
                            // [31:16] Reserved
                            // [15:0]  Buffer length
    
    uint32_t ds_ctl1;       // Control word 1 (reserved)
    
    uint32_t ds_hw0;        // Hardware status 0
                            // [31]    Done
                            // [30:16] Reserved
                            // [15]    CRC error
                            // [14]    PHY error
                            // [13]    Decrypt error
                            // [12]    MIC error
                            // [11:0]  Reserved
    
    uint32_t ds_hw1;        // Hardware status 1
                            // [31:16] Reserved
                            // [15:0]  Frame length
    
    uint32_t ds_hw2;        // Hardware status 2
                            // [31:24] Rate index
                            // [23:16] Antenna
                            // [15:8]  RSSI (combined)
                            // [7:0]   Reserved
    
    uint32_t ds_hw3;        // Hardware status 3
                            // [31:0]  RX timestamp
};
```

## 3. Register Reference

### 3.1 Core MAC Registers

#### AR_CR (0x0008) - Command Register
```
Bit 31-7:  Reserved
Bit 6:     SWI - Software interrupt
Bit 5:     RXD - RX disable
Bit 4:     Reserved
Bit 3:     Reserved
Bit 2:     RXE - RX enable
Bit 1:     Reserved
Bit 0:     Reserved
```

#### AR_ISR (0x0080) - Interrupt Status Register
```
Bit 31-19: Reserved
Bit 18:    BMISS - Beacon miss
Bit 17:    BRSSI - Beacon RSSI below threshold
Bit 16:    SWBA - Software beacon alert
Bit 15:    RXKCM - RX key cache miss
Bit 14:    RXPHY - RX PHY error
Bit 13:    SWI - Software interrupt
Bit 12:    MIB - MIB counter overflow
Bit 11:    TXURN - TX underrun
Bit 10:    TXEOL - TX end of list
Bit 9:     TXNOPKT - TX no packet
Bit 8:     TXERR - TX error
Bit 7:     TXDESC - TX descriptor request
Bit 6:     TXOK - TX OK
Bit 5:     RXORN - RX overrun
Bit 4:     RXEOL - RX end of list
Bit 3:     RXNOPKT - RX no packet
Bit 2:     RXERR - RX error
Bit 1:     RXDESC - RX descriptor request
Bit 0:     RXOK - RX OK
```

### 3.2 PHY Registers

#### AR_PHY_TURBO (0x9804) - Turbo/HT Mode
```
Bit 31-9:  Reserved
Bit 8:     SINGLE_HT_LTF1 - Single HT-LTF in short GI
Bit 7:     SHORT_GI_40 - Short guard interval for HT40
Bit 6:     HT_EN - High throughput enable
Bit 5:     DYN2040_EXT_CH - Dynamic 20/40 extension channel
Bit 4:     DYN2040_PRI_CH - Dynamic 20/40 primary channel
Bit 3:     DYN2040_PRI_ONLY - Dynamic 20/40 primary only
Bit 2:     DYN2040_EN - Dynamic 20/40 enable
Bit 1:     TURBO_SHORT - Turbo short
Bit 0:     TURBO_MODE - Turbo mode enable
```

#### AR_PHY_CCA (0x9864) - Clear Channel Assessment
```
Bit 31-18: Reserved
Bit 17-10: THRESH62 - Threshold for extension channel
Bit 9-2:   THRESH - CCA threshold
Bit 1-0:   MINCCA_PWR - Minimum CCA power
```

## 4. Initialization Sequences

### 4.1 Cold Start Sequence

```
1. PCI Configuration
   ├─ Enable memory space (PCI_COMMAND[1])
   ├─ Enable bus master (PCI_COMMAND[2])
   └─ Map BAR0 to kernel virtual address

2. Hardware Reset
   ├─ Write IER = 0 (disable interrupts)
   ├─ Write IMR = 0 (mask all interrupts)
   ├─ Read ISR (clear pending)
   ├─ Write CR = RXD (disable RX)
   ├─ Wait 100ms
   └─ Write CFG = 0 (default config)

3. EEPROM Read
   ├─ Read MAC address (offset 0x1D)
   ├─ Read calibration data
   └─ Read regulatory domain

4. PHY Initialization
   ├─ Clear RF silent (PHY_TEST)
   ├─ Write PHY_SETTLING
   ├─ Write PHY_DESIRED_SZ
   ├─ Write PHY_AGC_CTL1
   ├─ Write PHY_CCA
   ├─ Write PHY_SFCORR_LOW
   ├─ Write PHY_SFCORR
   ├─ Enable HT mode (PHY_TURBO[6])
   └─ Initialize RF synthesizer

5. Calibration
   ├─ Noise floor calibration
   ├─ IQ calibration
   ├─ ADC gain/DC offset calibration
   └─ PA linearization (if supported)

6. Queue Setup
   ├─ Allocate TX descriptor rings (4 queues)
   ├─ Allocate RX descriptor ring
   ├─ Allocate DMA buffers
   ├─ Write RXDP (RX descriptor pointer)
   ├─ Write TXDPs (TX descriptor pointers)
   ├─ Enable TX DMA (TXCFG[0])
   └─ Enable RX DMA (RXCFG[0])

7. Interrupt Setup
   ├─ Register IRQ handler
   ├─ Write IER = 1 (enable interrupts)
   └─ Write IMR = flags (unmask interrupts)

8. Start
   └─ Write CR = RXE (enable RX)
```

### 4.2 Channel Change Sequence

```
1. Prepare
   ├─ Disable TX queues
   ├─ Drain TX queues (wait for completion)
   └─ Disable RX (CR = RXD)

2. Change Channel
   ├─ Calculate frequency from channel number
   ├─ Program RF synthesizer
   │  ├─ Write synthesizer control registers
   │  ├─ Wait for PLL lock
   │  └─ Verify lock status
   └─ Update channel state

3. Calibrate
   ├─ Noise floor calibration
   ├─ IQ mismatch correction
   └─ Update TX power table

4. Resume
   ├─ Enable RX (CR = RXE)
   └─ Enable TX queues
```

## 5. Packet Processing

### 5.1 TX Packet Flow

```c
// Driver entry point
ath9k_tx(ieee80211_hw_t* hw, sk_buff_t* skb) {
    // 1. Select queue based on priority
    uint8_t qnum = select_tx_queue(skb);
    
    // 2. Get TX queue
    ath9k_tx_queue_t* txq = ah->tx_queues[qnum];
    
    // 3. Check queue space
    if (queue_full(txq)) {
        return -ENOSPC;
    }
    
    // 4. Allocate buffer
    void* buf = kmalloc(skb->len);
    memcpy(buf, skb->data, skb->len);
    
    // 5. Get descriptor
    uint16_t tail = txq->tail;
    ath9k_desc_t* desc = &txq->desc_ring[tail];
    
    // 6. Fill descriptor
    desc->ds_data = virt_to_phys(buf);
    desc->ds_ctl0 = skb->len;
    desc->ds_ctl1 = ATH9K_TXDESC_INTREQ | rate_to_hw_rate(rate);
    desc->ds_link = get_next_desc_addr(txq, tail);
    
    // 7. Update tail
    txq->tail = (tail + 1) % txq->size;
    txq->buffers[tail] = buf;
    
    // 8. Notify hardware (ring doorbell)
    ath9k_hw_reg_write(ah, AR_QTXDP(qnum), desc->ds_link);
    
    return 0;
}
```

### 5.2 RX Packet Flow

```c
// Interrupt handler calls this
ath9k_rx_tasklet(ath9k_softc_t* sc) {
    ath9k_rx_queue_t* rxq = ah->rx_queue;
    
    while (frames_available(rxq)) {
        // 1. Get descriptor
        uint16_t head = rxq->head;
        ath9k_desc_t* desc = &rxq->desc_ring[head];
        
        // 2. Check done bit
        if (!(desc->ds_hw0 & ATH9K_RXDESC_DONE)) {
            break;  // No more frames
        }
        
        // 3. Check errors
        if (desc->ds_hw0 & ERROR_MASK) {
            // Handle error
            ah->rx_errors++;
            goto next;
        }
        
        // 4. Extract frame info
        uint16_t len = desc->ds_hw1 & 0xFFFF;
        uint8_t rate = (desc->ds_hw2 >> 24) & 0xFF;
        int8_t rssi = (desc->ds_hw2 >> 8) & 0xFF;
        
        // 5. Get buffer
        void* buf = rxq->buffers[head];
        
        // 6. Create SKB
        sk_buff_t skb;
        skb.data = buf;
        skb.len = len;
        
        // 7. Fill RX status
        ieee80211_rx_status_t rx_status;
        rx_status.rate_idx = hw_rate_to_mac80211(rate);
        rx_status.signal = rssi;
        rx_status.freq = ah->curchan;
        skb.cb = &rx_status;
        
        // 8. Pass to mac80211
        ieee80211_rx(sc->hw, &skb);
        
        // 9. Update statistics
        ah->rx_packets++;
        
next:
        // 10. Clear done bit
        desc->ds_hw0 &= ~ATH9K_RXDESC_DONE;
        
        // 11. Advance head
        rxq->head = (head + 1) % rxq->size;
    }
}
```

## 6. Calibration Algorithms

### 6.1 Noise Floor Calibration

**Purpose**: Measure background noise level to set optimal CCA threshold.

**Algorithm**:
```
1. Disable AGC
2. Set RX gain to minimum
3. Collect 1024 samples
4. Calculate mean power: NF = 10 * log10(mean_power)
5. Update CCA threshold = NF + margin
6. Re-enable AGC
```

**Implementation**:
```c
void ath9k_calibrate_nf(ath9k_hw_t* ah) {
    // Disable AGC
    uint32_t agc = ath9k_hw_reg_read(ah, AR_PHY_AGC_CONTROL);
    ath9k_hw_reg_write(ah, AR_PHY_AGC_CONTROL, agc & ~0x1);
    
    // Start NF calibration
    ath9k_hw_reg_write(ah, AR_PHY_AGC_CONTROL, agc | 0x2);
    
    // Wait for completion (poll status register)
    int timeout = 10000;
    while (timeout--) {
        uint32_t status = ath9k_hw_reg_read(ah, AR_PHY_AGC_CONTROL);
        if (status & 0x4) {  // Calibration complete
            break;
        }
        udelay(10);
    }
    
    // Read NF value
    int16_t nf = (int16_t)(ath9k_hw_reg_read(ah, AR_PHY_CCA) >> 16);
    nf = (nf >> 1) & 0xFF;  // Convert to dBm
    nf = -nf;
    
    ah->noise_floor = nf;
    
    // Re-enable AGC
    ath9k_hw_reg_write(ah, AR_PHY_AGC_CONTROL, agc);
}
```

### 6.2 IQ Calibration

**Purpose**: Correct I/Q imbalance in the receiver.

**Theory**:
- I (In-phase) and Q (Quadrature) should be orthogonal
- Hardware imperfections cause amplitude/phase imbalance
- Calibration measures and corrects this imbalance

**Steps**:
1. Inject known tone at IF
2. Measure received I and Q
3. Calculate correction factors
4. Apply correction to RX chain

## 7. Power Management

### 7.1 TX Power Control

**Regulatory Limits**:
- FCC (US): 20 dBm (100 mW) for 2.4 GHz
- ETSI (EU): 20 dBm (100 mW) for 2.4 GHz
- MKK (Japan): 10 dBm (10 mW) for indoor

**TX Power Table**:
```c
struct tx_power_entry {
    uint8_t rate_idx;
    int8_t power_dbm;
    uint8_t pa_config;  // Power amplifier configuration
};

// Example for AR9285
tx_power_entry tx_power_table_2ghz[] = {
    { 0,  20, 0x3F },  // 1 Mbps
    { 1,  20, 0x3F },  // 2 Mbps
    { 4,  20, 0x3F },  // 6 Mbps
    { 11, 19, 0x3C },  // 54 Mbps (slightly lower for linearity)
    { 15, 18, 0x38 },  // MCS7 (40 MHz)
};
```

### 7.2 Power Save Modes

**CAM (Constant Awake Mode)**:
- Always awake
- No power saving
- Lowest latency

**Network Sleep**:
- Sleep between beacons
- Wake for DTIM
- Medium power saving

**Max Power Save**:
- Sleep most of the time
- Wake only for directed frames
- Maximum power saving, higher latency

## 8. Performance Optimization

### 8.1 Descriptor Ring Sizing

**TX Queue Size**: 256 descriptors
- Sufficient for burst traffic
- Not too large (memory waste)
- Calculated: max_burst_size / avg_frame_size

**RX Queue Size**: 512 descriptors
- Handle bursty RX traffic
- Prevent overruns
- Must be >= 2 * max_a_mpdu_size

### 8.2 Interrupt Coalescing

**Problem**: Too many interrupts reduce throughput

**Solution**: Interrupt coalescing
```c
// Fire interrupt every N frames or after timeout
#define ATH9K_INT_COALESCE_FRAMES  64
#define ATH9K_INT_COALESCE_USEC    500

ath9k_hw_reg_write(ah, AR_RIMT, ATH9K_INT_COALESCE_USEC);
ath9k_hw_reg_write(ah, AR_RIMC, ATH9K_INT_COALESCE_FRAMES);
```

### 8.3 DMA Burst Size

**Optimal burst size**: 128 bytes (2 cache lines)
```c
uint32_t dma_cfg = ath9k_hw_reg_read(ah, AR_DMA_CONFIG);
dma_cfg &= ~AR_DMA_BURST_MASK;
dma_cfg |= AR_DMA_BURST_128;
ath9k_hw_reg_write(ah, AR_DMA_CONFIG, dma_cfg);
```

## 9. Error Handling

### 9.1 RX Errors

| Error | Cause | Action |
|-------|-------|--------|
| CRC Error | Bit errors in frame | Drop frame, increment counter |
| PHY Error | Signal too weak/distorted | Drop frame, check RSSI |
| Decrypt Error | Wrong key or corrupted | Drop frame, check key |
| MIC Error | TKIP MIC failure | Report to supplicant, possible attack |
| FIFO Overrun | RX ring full | Drop frame, increase ring size |

### 9.2 TX Errors

| Error | Cause | Action |
|-------|-------|--------|
| Excessive Retries | Channel busy/bad link | Report failure to mac80211 |
| Underrun | DMA too slow | Adjust DMA burst, check bus |
| FIFO Underrun | Frame not ready | Retry or drop |
| Filtered | Filtered by HW | Normal for some frames |

## 10. Regulatory Compliance

### 10.1 Country Codes

```c
struct regulatory_domain {
    char country[2];
    uint8_t max_power_2ghz;  // dBm
    uint8_t max_power_5ghz;  // dBm
    uint16_t allowed_channels_2ghz;  // Bitmask
    uint16_t allowed_channels_5ghz;  // Bitmask
};

regulatory_domain rd_table[] = {
    { "US", 20, 23, 0x7FF, 0x1F1F },   // FCC
    { "GB", 20, 23, 0x7FF, 0x1F1F },   // ETSI
    { "JP", 10, 10, 0x3FFF, 0x0F0F },  // MKK
    { "CN", 20, 23, 0x7FF, 0x1F1F },   // China
};
```

### 10.2 DFS (Dynamic Frequency Selection)

Required for 5 GHz in some regions to avoid radar interference.

**Radar Detection**:
1. Monitor channel for radar pulses
2. If detected, mark channel as unavailable
3. Switch to different channel
4. Wait 30 minutes before re-checking

---

**Document Version**: 1.0  
**Last Updated**: 2026-05-26  
**Status**: Complete
