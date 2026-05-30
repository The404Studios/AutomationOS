# AutomationOS Driver Expansion Plan (Phase 2-3)

**Date:** 2026-05-26  
**Status:** Draft  
**Version:** 1.0  
**Phase:** 2-3 Driver Development  
**Estimated Duration:** 12-16 weeks

---

## Executive Summary

This document outlines the driver expansion roadmap for AutomationOS Phase 2-3, building on the Phase 1 foundation (serial, PS/2, framebuffer, PIT). The goal is to achieve Linux-level hardware compatibility across storage, networking, graphics, audio, USB, and wireless subsystems.

**Key Objectives:**
- Production-grade storage support (NVMe, AHCI/SATA)
- Network connectivity (Intel e1000, Realtek 8169)
- Modern graphics (Intel i915, AMD/NVIDIA modesetting)
- Audio support (HDA/AC97)
- USB infrastructure (XHCI, HID devices)
- Wireless networking (ath9k, iwlwifi)

**Driver Framework Philosophy:**
- Unified driver model with probe/remove/suspend/resume lifecycle
- AI telemetry hooks in every driver (latency, throughput, errors, power)
- Hot-plug support for USB/PCIe devices
- Modular architecture (built-in + loadable kernel modules)

---

## 1. Storage Subsystem

### 1.1 NVMe Driver

**Priority:** High (required for modern storage)  
**Complexity:** Medium-High  
**Estimated Duration:** 3-4 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/storage/
├── nvme/
│   ├── nvme_core.c       # Core NVMe driver
│   ├── nvme_pci.c        # PCI interface
│   ├── nvme_queue.c      # Queue management (SQ/CQ)
│   ├── nvme_admin.c      # Admin command handling
│   ├── nvme_io.c         # I/O command handling
│   └── nvme_namespace.c  # Namespace management
├── block_layer.c         # Generic block I/O
└── partition.c           # Partition table parsing (GPT/MBR)
```

**Key Features:**
- PCIe-based NVMe 1.3+ support
- Admin queue setup (Identify, Create/Delete I/O Queues)
- Multiple I/O queues (one per CPU core for parallelism)
- Submission Queue (SQ) and Completion Queue (CQ) management
- Namespace discovery and management
- Read/Write command submission
- Interrupt handling (MSI-X preferred, MSI fallback)
- DMA buffer management (PRP lists for data transfer)

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* bar0;              // BAR0 (controller registers)
    uint64_t doorbell_stride;
    
    // Admin queue
    nvme_queue_t* admin_queue;
    
    // I/O queues (one per CPU)
    nvme_queue_t** io_queues;
    uint16_t num_io_queues;
    
    // Device capabilities
    uint32_t max_transfer_size;
    uint16_t page_size;
    
    // Namespaces
    nvme_namespace_t** namespaces;
    uint32_t num_namespaces;
    
    // Telemetry
    telemetry_t* metrics;
} nvme_controller_t;

typedef struct {
    void* submission_queue;    // SQ (host writes)
    void* completion_queue;    // CQ (host reads)
    uint32_t* sq_doorbell;     // SQ doorbell register
    uint32_t* cq_doorbell;     // CQ doorbell register
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t queue_depth;
    spinlock_t lock;
} nvme_queue_t;

typedef struct {
    uint32_t nsid;             // Namespace ID
    uint64_t size_bytes;
    uint32_t block_size;
    block_device_t* block_dev; // Generic block device interface
} nvme_namespace_t;
```

**Driver Flow:**
1. **Probe:** Detect NVMe controller via PCI enumeration (vendor 0x8086, class 0x010802)
2. **Initialize:**
   - Map BAR0 (controller registers)
   - Reset controller (CC.EN = 0, wait for CSTS.RDY = 0)
   - Setup admin queue (ASQB/ACQB registers)
   - Enable controller (CC.EN = 1, wait for CSTS.RDY = 1)
   - Read controller capabilities (CAP register)
3. **Admin Commands:**
   - Identify Controller (get max queues, transfer size)
   - Create I/O Completion Queues (one per CPU)
   - Create I/O Submission Queues (one per CPU)
   - Identify Namespaces (discover all available namespaces)
4. **Namespace Setup:**
   - For each namespace: register as block device in VFS
   - Setup I/O request queue
5. **I/O Operations:**
   - Read/Write commands submitted to I/O SQ
   - Ring SQ doorbell to notify controller
   - Interrupt on CQ completion
   - Parse completion entry, complete I/O request

**Dependencies:**
- PCI subsystem (enumerate NVMe controllers)
- DMA allocator (allocate physically contiguous buffers for queues)
- Interrupt subsystem (MSI-X/MSI)
- Block layer (register block devices)

**Testing Approach:**
- QEMU with NVMe emulation: `-drive file=disk.img,if=none,id=nvm -device nvme,serial=deadbeef,drive=nvm`
- Unit tests: queue management, admin commands, namespace discovery
- Integration tests: read/write 4KB blocks, sequential/random I/O
- Stress tests: parallel I/O from multiple threads, queue depth saturation
- Real hardware: Intel 660p, Samsung 970 EVO, WD Black SN750

**AI Integration:**
- Telemetry: I/O latency (per command), queue depth utilization, error rates, throughput (MB/s)
- Adaptive tuning: Queue depth, interrupt coalescing, read-ahead size
- Anomaly detection: Unusual error rates, increasing latency, failing drives

**Estimated Complexity:** 7/10  
**Lines of Code:** ~2,500  
**Critical Path:** Admin queue setup, I/O queue management, DMA handling

---

### 1.2 AHCI/SATA Driver

**Priority:** High (legacy storage support)  
**Complexity:** Medium  
**Estimated Duration:** 2-3 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/storage/
├── ahci/
│   ├── ahci_core.c       # Core AHCI driver
│   ├── ahci_pci.c        # PCI interface
│   ├── ahci_port.c       # Port management
│   ├── ahci_cmd.c        # Command handling
│   └── ahci_fis.c        # FIS (Frame Information Structure) handling
└── ata/
    ├── ata_identify.c    # ATA IDENTIFY command
    ├── ata_pio.c         # PIO mode (legacy)
    └── ata_dma.c         # DMA mode
```

**Key Features:**
- AHCI 1.3+ support (Serial ATA Advanced Host Controller Interface)
- SATA III support (6 Gbps)
- Port multiplier support (up to 15 devices per port)
- Native Command Queuing (NCQ) for concurrent commands
- Hot-plug detection (SATA port events)
- Power management (ALPM - Aggressive Link Power Management)

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* abar;              // AHCI Base Address (BAR5)
    uint32_t ports_implemented;
    ahci_port_t* ports[32];  // Max 32 ports
    uint32_t num_ports;
    
    // Capabilities
    bool supports_ncq;
    bool supports_64bit;
    uint32_t num_command_slots;
    
    telemetry_t* metrics;
} ahci_controller_t;

typedef struct {
    ahci_controller_t* controller;
    uint32_t port_num;
    void* port_regs;         // Port registers (HBA_PORT)
    
    // Command engine
    void* command_list;      // Command list (32 entries)
    void* fis_base;          // Received FIS
    void* command_tables[32]; // Command tables
    
    // Device info
    bool device_present;
    bool is_atapi;           // CD/DVD drive
    uint64_t size_sectors;
    uint32_t sector_size;
    
    block_device_t* block_dev;
    spinlock_t lock;
} ahci_port_t;
```

**Driver Flow:**
1. **Probe:** Detect AHCI controller via PCI (class 0x010601)
2. **Initialize:**
   - Map ABAR (BAR5, AHCI registers)
   - Enable AHCI mode (GHC.AE = 1)
   - Read CAP register (capabilities)
   - Enumerate ports (PI register - Ports Implemented)
3. **Port Setup:**
   - Allocate command list, FIS, command tables
   - Start command engine (PxCMD.ST = 1, PxCMD.FRE = 1)
   - Issue SATA COMRESET (detect device)
4. **Device Identification:**
   - Send ATA IDENTIFY DEVICE command (via H2D FIS)
   - Read response (device model, size, capabilities)
   - Register as block device
5. **I/O Operations:**
   - Build command FIS (H2D Register FIS)
   - Setup PRD (Physical Region Descriptor) for DMA
   - Write command header to command list
   - Set PxCI (Command Issue) bit
   - Wait for interrupt (PxIS.DHRS or PxIS.TFES)
   - Parse D2H FIS for status

**Dependencies:**
- PCI subsystem
- DMA allocator (32-bit and 64-bit addressing)
- Interrupt subsystem
- Block layer

**Testing Approach:**
- QEMU with AHCI: `-drive file=disk.img,if=none,id=sata0 -device ahci,id=ahci -device ide-hd,drive=sata0,bus=ahci.0`
- Unit tests: Port detection, FIS parsing, command submission
- Integration tests: Sequential/random read/write, NCQ (multiple outstanding commands)
- Real hardware: Intel SATA controllers, AMD SATA, legacy IDE-to-SATA bridges

**AI Integration:**
- Telemetry: Command latency, NCQ queue depth, error rates, SMART data (temperature, reallocated sectors)
- Predictive maintenance: Detect failing drives from SMART trends
- Adaptive tuning: Read-ahead size, NCQ depth, link power states

**Estimated Complexity:** 6/10  
**Lines of Code:** ~2,000  
**Critical Path:** FIS handling, DMA setup, NCQ implementation

---

## 2. Network Subsystem

### 2.1 Intel e1000 Driver

**Priority:** High (ubiquitous in VMs and real hardware)  
**Complexity:** Medium  
**Estimated Duration:** 2-3 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/net/
├── e1000/
│   ├── e1000_main.c      # Core driver
│   ├── e1000_hw.c        # Hardware access
│   ├── e1000_tx.c        # Transmit logic
│   ├── e1000_rx.c        # Receive logic
│   ├── e1000_phy.c       # PHY management
│   └── e1000_eeprom.c    # EEPROM access (MAC address)
├── netdev.c              # Network device abstraction
└── ethernet.c            # Ethernet frame handling
```

**Key Features:**
- Intel 82540, 82545, 82574 (e1000/e1000e family)
- 1 Gbps Ethernet
- Scatter-gather DMA
- Hardware checksum offload (IP, TCP, UDP)
- Receive-side scaling (RSS) for multi-core
- VLAN tagging support
- Link status detection

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* mmio_base;         // BAR0 (MMIO registers)
    
    // MAC address
    uint8_t mac_addr[6];
    
    // TX ring
    void* tx_ring;           // TX descriptor ring
    void** tx_buffers;       // TX buffer pointers
    uint32_t tx_tail;
    uint32_t tx_head;
    uint32_t tx_ring_size;
    
    // RX ring
    void* rx_ring;           // RX descriptor ring
    void** rx_buffers;       // RX buffer pointers
    uint32_t rx_tail;
    uint32_t rx_head;
    uint32_t rx_ring_size;
    
    // Link state
    bool link_up;
    uint32_t link_speed;     // 10/100/1000
    bool full_duplex;
    
    // Network device
    netdev_t* netdev;
    
    spinlock_t tx_lock;
    telemetry_t* metrics;
} e1000_adapter_t;

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t cso;            // Checksum offset
    uint8_t cmd;             // Command
    uint8_t status;          // Status
    uint16_t css;            // Checksum start
    uint16_t special;
} PACKED e1000_tx_desc_t;

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} PACKED e1000_rx_desc_t;
```

**Driver Flow:**
1. **Probe:** Detect e1000 via PCI (vendor 0x8086, device 0x100E/0x10D3/etc.)
2. **Initialize:**
   - Map MMIO BAR0
   - Reset controller (CTRL.RST)
   - Read MAC address from EEPROM
   - Setup TX/RX descriptor rings
   - Configure interrupts (ICR/IMS)
3. **TX Setup:**
   - Allocate circular descriptor ring (256 entries)
   - Set TDBAL/TDBAH (ring base address)
   - Set TDLEN (ring length)
   - Set TDH=0, TDT=0 (head/tail)
   - Enable TX (TCTL.EN)
4. **RX Setup:**
   - Allocate circular descriptor ring (256 entries)
   - Allocate 2KB buffers for each descriptor
   - Set RDBAL/RDBAH (ring base address)
   - Set RDLEN (ring length)
   - Set RDH=0, RDT=(RDLEN-1) (tail one behind head)
   - Enable RX (RCTL.EN)
5. **Transmit:**
   - Copy packet to TX buffer
   - Fill TX descriptor (buffer address, length, EOP, IFCS)
   - Advance TDT (tail)
   - Interrupt on TX completion (DD bit set)
6. **Receive:**
   - Interrupt on RX (ICR.RXT0)
   - Read RX descriptor at RDH
   - Copy packet from RX buffer
   - Refill buffer, advance RDH
   - Pass packet to network stack

**Dependencies:**
- PCI subsystem
- DMA allocator
- Interrupt subsystem
- Network stack (Ethernet, IP, TCP/UDP)

**Testing Approach:**
- QEMU with e1000: `-netdev user,id=net0 -device e1000,netdev=net0`
- Unit tests: Descriptor ring management, MAC address parsing
- Integration tests: DHCP, ping, TCP connection
- Performance tests: iperf3 (1 Gbps throughput)
- Real hardware: Intel Pro/1000 NICs

**AI Integration:**
- Telemetry: TX/RX packets, bytes, errors, dropped packets, interrupt rate
- Adaptive tuning: Interrupt coalescing, ring buffer sizes, checksum offload
- Anomaly detection: Packet loss spikes, collision storms, link flapping

**Estimated Complexity:** 5/10  
**Lines of Code:** ~1,800  
**Critical Path:** Descriptor ring management, interrupt handling

---

### 2.2 Realtek 8169 Driver

**Priority:** Medium (common in consumer hardware)  
**Complexity:** Medium  
**Estimated Duration:** 2 weeks

#### Architecture Design

**Similar to e1000, adapted for Realtek:**
- PCI vendor 0x10EC, device 0x8169
- TX/RX descriptor rings
- Hardware checksum offload
- VLAN support
- Different register layout (PHY, link detection)

**Key Differences from e1000:**
- Integrated PHY (no external EEPROM for MAC)
- Different interrupt flags (ISR/IMR registers)
- Proprietary descriptor format
- Realtek-specific errata and quirks

**Estimated Complexity:** 5/10  
**Lines of Code:** ~1,500

---

## 3. Graphics Subsystem

### 3.1 Intel i915 Driver

**Priority:** High (most common integrated GPU)  
**Complexity:** Very High  
**Estimated Duration:** 6-8 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/gpu/
├── drm/                  # Direct Rendering Manager (kernel framework)
│   ├── drm_core.c        # DRM core
│   ├── drm_mode.c        # Modesetting (KMS)
│   ├── drm_gem.c         # Graphics Execution Manager (memory)
│   ├── drm_fb.c          # Framebuffer emulation
│   └── drm_atomic.c      # Atomic modesetting
├── i915/
│   ├── i915_drv.c        # Main driver
│   ├── i915_gem.c        # GEM (GPU memory)
│   ├── i915_display.c    # Display engine
│   ├── i915_irq.c        # Interrupt handling
│   ├── i915_gt.c         # Graphics Technology (GPU)
│   ├── intel_dp.c        # DisplayPort
│   ├── intel_hdmi.c      # HDMI
│   ├── intel_lvds.c      # LVDS (laptop panels)
│   └── intel_ddi.c       # Digital Display Interface
└── edid/
    └── edid_parser.c     # EDID parsing (monitor info)
```

**Key Features:**
- Intel HD Graphics 4000+ (Gen 7+)
- Kernel Mode Setting (KMS)
- GEM (buffer management, GPU memory allocation)
- DisplayPort 1.2, HDMI 1.4/2.0, eDP (embedded DisplayPort)
- Multiple displays (up to 3 simultaneous)
- Hardware cursor
- DPMS (power management)
- VBlank handling

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* gtt;               // Graphics Translation Table (BAR0)
    void* regs;              // MMIO registers (BAR2)
    
    // Display outputs
    intel_output_t* outputs[8];
    uint32_t num_outputs;
    
    // Planes (framebuffers)
    intel_plane_t* planes[4];
    
    // GEM objects
    list_t* gem_objects;
    spinlock_t gem_lock;
    
    // Interrupts
    uint32_t irq_mask;
    
    drm_device_t* drm_dev;
    telemetry_t* metrics;
} i915_device_t;

typedef struct {
    uint64_t phys_addr;
    uint64_t size;
    void* virt_addr;
    uint32_t handle;
    refcount_t refcount;
} i915_gem_object_t;

typedef struct {
    i915_device_t* dev;
    uint32_t port;           // PORT_A/B/C/D/E
    bool connected;
    
    // EDID
    uint8_t edid[128];
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
    
    // Active mode
    display_mode_t* mode;
} intel_output_t;
```

**Driver Flow:**
1. **Probe:** Detect Intel GPU via PCI (vendor 0x8086, class 0x030000)
2. **Initialize:**
   - Map GTT (BAR0, graphics translation table)
   - Map MMIO (BAR2, registers)
   - Reset display engine
   - Setup GEM (allocate GTT entries)
   - Enable power wells
3. **Modesetting:**
   - Detect outputs (walk through ports)
   - Read EDID from connected monitors (via I2C/DDC)
   - Parse EDID (native resolution, supported modes)
   - Program display engine (pipe, plane, transcoder)
   - Enable VBlank interrupts
4. **Framebuffer Setup:**
   - Allocate GEM object for framebuffer
   - Pin to GTT
   - Configure plane (base address, stride, format)
   - Enable display
5. **Rendering (Phase 4):**
   - Command buffer submission
   - Execbuffer (GPU command execution)
   - Fence/sync objects

**Dependencies:**
- PCI subsystem
- I2C subsystem (for EDID reading)
- DRM core
- GTT allocator

**Testing Approach:**
- QEMU with i915 emulation: Limited support, use virtio-gpu for basic testing
- Real hardware: Intel HD 4000/5000/6000, Iris, UHD Graphics
- Unit tests: EDID parsing, mode validation
- Integration tests: Modesetting, multiple displays, VBlank
- Stress tests: Mode changes, hot-plug/unplug

**AI Integration:**
- Telemetry: Frame rate, VBlank events, mode switches, GPU hangs
- Adaptive tuning: Power state transitions, panel fitter settings
- Anomaly detection: GPU lockups, display glitches

**Estimated Complexity:** 9/10  
**Lines of Code:** ~8,000  
**Critical Path:** Modesetting, GEM, display power management

**Note:** i915 is extremely complex. Consider starting with simpler framebuffer driver and basic modesetting, then incrementally add features.

---

### 3.2 AMD/NVIDIA Modesetting

**Priority:** Medium (wider hardware support)  
**Complexity:** Very High  
**Estimated Duration:** 8-10 weeks (per vendor)

#### Architecture Design

**AMD (AMDGPU):**
- Support for GCN+ architectures (R9 200 series onward)
- Display Core Next (DCN) modesetting
- Similar to i915 but with different register layout
- Open-source kernel driver available (can port code)

**NVIDIA (Nouveau):**
- Open-source reverse-engineered driver
- Limited to modesetting (no 3D acceleration without signed firmware)
- Support for GeForce 8+ (Tesla, Fermi, Kepler, Maxwell, Pascal, Turing)
- Signed firmware required for GPU context switching on newer cards

**Recommendation:** Start with AMD due to better open-source documentation. NVIDIA only for basic modesetting.

**Estimated Complexity:** 9/10 each  
**Lines of Code:** ~10,000 per vendor

---

## 4. Audio Subsystem

### 4.1 Intel HDA (High Definition Audio)

**Priority:** Medium  
**Complexity:** High  
**Estimated Duration:** 4-5 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/audio/
├── hda/
│   ├── hda_core.c        # Core HDA controller
│   ├── hda_codec.c       # Codec management
│   ├── hda_jack.c        # Jack detection
│   ├── hda_pcm.c         # PCM (audio streams)
│   └── hda_generic.c     # Generic codec parser
└── pcm/
    ├── pcm_core.c        # PCM abstraction
    ├── pcm_buffer.c      # Ring buffer
    └── pcm_dma.c         # DMA handling
```

**Key Features:**
- Intel HDA specification (HD Audio)
- Codec enumeration (via HD Audio Link)
- Multiple streams (playback, capture, up to 16 simultaneous)
- Sample rates: 8 kHz - 192 kHz
- Formats: 16/24/32-bit PCM, AC3 passthrough
- Volume control, mute, jack detection

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* mmio_base;         // BAR0
    
    // CORB/RIRB (command/response buffers)
    void* corb;              // Command Output Ring Buffer
    void* rirb;              // Response Input Ring Buffer
    uint32_t corb_wp;
    uint32_t rirb_rp;
    
    // Codecs
    hda_codec_t* codecs[15]; // Max 15 codecs
    uint32_t num_codecs;
    
    // Streams
    hda_stream_t* streams[16];
    
    telemetry_t* metrics;
} hda_controller_t;

typedef struct {
    hda_controller_t* controller;
    uint8_t addr;            // Codec address (0-14)
    uint32_t vendor_id;
    uint32_t subsystem_id;
    
    // Audio function groups
    hda_afg_t* afg;
    
    // Nodes (widgets)
    hda_node_t** nodes;
    uint32_t num_nodes;
} hda_codec_t;

typedef struct {
    hda_controller_t* controller;
    uint32_t stream_tag;
    uint32_t format;         // Sample rate, bits, channels
    void* bdl;               // Buffer Descriptor List
    void* buffer;            // DMA buffer
    uint32_t buffer_size;
    bool running;
} hda_stream_t;
```

**Driver Flow:**
1. **Probe:** Detect HDA controller via PCI (vendor varies, class 0x040300)
2. **Initialize:**
   - Map MMIO BAR0
   - Reset controller (GCTL.CRST)
   - Setup CORB/RIRB (command/response buffers)
   - Enable interrupts
3. **Codec Enumeration:**
   - Read STATESTS register (codec presence)
   - For each codec: send GET_PARAMETER(VENDOR_ID)
   - Identify codec model
4. **Codec Configuration:**
   - Enumerate nodes (audio widgets)
   - Build routing graph (DAC -> mixer -> pin)
   - Configure pin widgets (enable output, set EAPD)
   - Setup default volumes
5. **Playback:**
   - Allocate DMA buffer (4KB pages)
   - Build BDL (buffer descriptor list)
   - Configure stream format (48 kHz, 16-bit, stereo)
   - Start DMA (SD0CTL.RUN)
   - Fill buffer with audio samples
   - Interrupt on buffer completion

**Dependencies:**
- PCI subsystem
- DMA allocator
- Interrupt subsystem
- Audio subsystem (ALSA-like API)

**Testing Approach:**
- QEMU with HDA: `-device intel-hda -device hda-duplex`
- Unit tests: Codec detection, node enumeration, format parsing
- Integration tests: Play sine wave, record from microphone
- Real hardware: Intel HDA chipsets on laptops/desktops

**AI Integration:**
- Telemetry: Buffer underruns, sample rate mismatches, jack events
- Adaptive tuning: Buffer sizes, DMA scheduling
- Anomaly detection: Audio glitches, codec hangs

**Estimated Complexity:** 7/10  
**Lines of Code:** ~3,000  
**Critical Path:** Codec initialization, stream setup, DMA handling

---

### 4.2 AC97 Driver

**Priority:** Low (legacy hardware)  
**Complexity:** Medium  
**Estimated Duration:** 1-2 weeks

#### Architecture Design

**Simpler than HDA:**
- Fixed codec layout (no enumeration needed)
- PIO or DMA-based I/O
- Single playback/capture stream
- 48 kHz maximum sample rate

**Use Case:** Older hardware, VirtualBox VMs

**Estimated Complexity:** 4/10  
**Lines of Code:** ~800

---

## 5. USB Subsystem

### 5.1 XHCI Controller Driver

**Priority:** High (USB 3.0+ required for modern devices)  
**Complexity:** Very High  
**Estimated Duration:** 6-8 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/usb/
├── core/
│   ├── usb_core.c        # USB core
│   ├── usb_device.c      # Device enumeration
│   ├── usb_hub.c         # Hub management
│   └── usb_urb.c         # USB Request Block
├── host/
│   ├── xhci/
│   │   ├── xhci_core.c   # XHCI controller
│   │   ├── xhci_ring.c   # Transfer/Event rings
│   │   ├── xhci_cmd.c    # Command ring
│   │   └── xhci_port.c   # Port management
│   ├── ehci/             # USB 2.0 (legacy)
│   └── uhci/             # USB 1.1 (legacy)
└── class/
    ├── hid/              # Human Interface Devices
    ├── storage/          # USB Mass Storage
    └── hub/              # USB hubs
```

**Key Features:**
- xHCI 1.0+ (Extensible Host Controller Interface)
- USB 3.2 Gen 2 (10 Gbps)
- USB 2.0/1.1 backward compatibility
- Multiple endpoints (up to 31 per device)
- Asynchronous I/O (Transfer Request Blocks - TRBs)
- Command, Transfer, and Event rings
- Root hub emulation

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* cap_regs;          // Capability registers
    void* op_regs;           // Operational registers
    void* runtime_regs;      // Runtime registers
    void* doorbell_array;    // Doorbell registers
    
    // Device Context Base Address Array
    void* dcbaap;
    
    // Command ring
    xhci_ring_t* cmd_ring;
    
    // Event ring
    xhci_event_ring_t* event_ring;
    
    // Ports
    uint32_t num_ports;
    xhci_port_t* ports[256];
    
    // Devices
    xhci_device_t* devices[256];
    
    telemetry_t* metrics;
} xhci_controller_t;

typedef struct {
    void* trbs;              // Transfer Request Blocks
    uint32_t enqueue_ptr;
    uint32_t dequeue_ptr;
    uint32_t cycle_bit;
    uint32_t num_trbs;
} xhci_ring_t;

typedef struct {
    xhci_controller_t* controller;
    uint8_t slot_id;
    uint8_t address;
    usb_speed_t speed;       // USB 1.1/2.0/3.x
    
    // Device context
    void* input_context;
    void* device_context;
    
    // Endpoints
    xhci_endpoint_t* endpoints[31];
} xhci_device_t;

typedef struct {
    xhci_ring_t* transfer_ring;
    uint8_t endpoint_num;
    usb_endpoint_type_t type; // Control/Bulk/Interrupt/Isoch
    uint16_t max_packet_size;
} xhci_endpoint_t;
```

**Driver Flow:**
1. **Probe:** Detect xHCI controller via PCI (class 0x0C0330)
2. **Initialize:**
   - Map capability, operational, runtime, doorbell registers
   - Reset controller (USBCMD.HCRST)
   - Allocate DCBAA (Device Context Base Address Array)
   - Allocate command ring, event ring
   - Program DCBAAP, CRCR (command ring), ERDP (event ring)
   - Enable controller (USBCMD.RUN)
3. **Port Enumeration:**
   - Read PORTSC registers (port status)
   - Detect connected devices (PORTSC.CCS)
   - Reset port (PORTSC.PR)
   - Wait for port enable (PORTSC.PED)
4. **Device Enumeration:**
   - Allocate slot (Enable Slot command)
   - Address device (Address Device command)
   - Read device descriptor (Control transfer, GET_DESCRIPTOR)
   - Read configuration descriptor
   - Set configuration (SET_CONFIGURATION)
   - Load class driver (HID, Storage, etc.)
5. **Data Transfer:**
   - Build TRBs (Transfer Request Blocks)
   - Enqueue TRBs on endpoint ring
   - Ring doorbell (notify controller)
   - Wait for event TRB (completion interrupt)

**Dependencies:**
- PCI subsystem
- DMA allocator (64-bit addressing)
- Interrupt subsystem (MSI-X)
- USB device drivers (HID, Storage)

**Testing Approach:**
- QEMU with xHCI: `-device qemu-xhci -device usb-kbd -device usb-storage,drive=usb0`
- Unit tests: Ring management, TRB encoding, command submission
- Integration tests: Enumerate USB keyboard, read from USB flash drive
- Real hardware: Intel/AMD xHCI controllers

**AI Integration:**
- Telemetry: Transfer rates, error counts, device hotplug events
- Adaptive tuning: Ring sizes, interrupt coalescing
- Anomaly detection: Device disconnects, transfer timeouts

**Estimated Complexity:** 9/10  
**Lines of Code:** ~5,000  
**Critical Path:** Ring management, TRB handling, device enumeration

---

### 5.2 USB HID Driver

**Priority:** High (keyboards, mice)  
**Complexity:** Medium  
**Estimated Duration:** 2 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/usb/class/hid/
├── hid_core.c            # HID core
├── hid_parser.c          # Report descriptor parser
├── hid_keyboard.c        # Keyboard handler
├── hid_mouse.c           # Mouse handler
└── hid_gamepad.c         # Gamepad handler
```

**Key Features:**
- HID 1.11 (Human Interface Device)
- Report descriptor parsing
- Boot protocol (simplified keyboard/mouse)
- Interrupt IN endpoint polling
- Multiple device types (keyboard, mouse, gamepad, touchpad)

**Data Structures:**
```c
typedef struct {
    usb_device_t* usb_dev;
    uint8_t interface_num;
    uint8_t endpoint_in;
    
    // HID descriptor
    uint8_t* report_descriptor;
    uint16_t report_desc_size;
    
    // Parsed report
    hid_report_t* report;
    
    // Input handler
    hid_device_type_t type;  // Keyboard/Mouse/Gamepad
    void (*input_handler)(uint8_t* data, uint16_t len);
    
    // Polling
    void* urb;               // Interrupt URB
    uint8_t poll_interval;   // In ms
} hid_device_t;

typedef struct {
    uint16_t usage_page;
    uint16_t usage;
    uint8_t report_id;
    uint8_t report_size;
    uint8_t report_count;
    // ... more fields
} hid_report_item_t;
```

**Driver Flow:**
1. **Probe:** Detect HID device (interface class 0x03)
2. **Initialize:**
   - Read HID descriptor
   - Fetch report descriptor (GET_DESCRIPTOR)
   - Parse report descriptor (identify input/output/feature reports)
   - Determine device type (keyboard: usage page 0x01, usage 0x06)
3. **Setup Interrupt Transfer:**
   - Allocate URB for interrupt IN endpoint
   - Submit URB (poll every N ms)
4. **Input Handling:**
   - On interrupt: read HID report (8 bytes for keyboard)
   - Parse report (modifier keys, scancodes)
   - Translate to input events (key press/release)
   - Send to input subsystem

**Dependencies:**
- USB core
- Input subsystem

**Testing Approach:**
- QEMU with USB keyboard: `-device usb-kbd`
- Unit tests: Report descriptor parsing
- Integration tests: Keyboard input, mouse movement
- Real hardware: USB keyboards, mice, gamepads

**AI Integration:**
- Telemetry: Input event rates, device latency
- Anomaly detection: Repeated key events (stuck key), unusual input patterns

**Estimated Complexity:** 5/10  
**Lines of Code:** ~1,200  
**Critical Path:** Report descriptor parsing, input event handling

---

## 6. Wireless Networking

### 6.1 ath9k Driver (Atheros 802.11n)

**Priority:** Medium  
**Complexity:** Very High  
**Estimated Duration:** 8-10 weeks

#### Architecture Design

**Components:**
```
kernel/drivers/net/wireless/
├── mac80211/             # Generic 802.11 stack
│   ├── mac80211_core.c   # Core MAC layer
│   ├── mac80211_tx.c     # Transmit
│   ├── mac80211_rx.c     # Receive
│   ├── mac80211_scan.c   # Scanning
│   └── mac80211_mlme.c   # MLME (authentication, association)
├── cfg80211/             # Configuration API
│   ├── cfg80211_core.c   # Core API
│   └── cfg80211_nl.c     # Netlink interface
└── ath/
    ├── ath9k/
    │   ├── ath9k_main.c  # Driver core
    │   ├── ath9k_hw.c    # Hardware access
    │   ├── ath9k_tx.c    # TX engine
    │   ├── ath9k_rx.c    # RX engine
    │   ├── ath9k_calib.c # Calibration
    │   └── ath9k_phy.c   # PHY management
    └── ath_common.c      # Common Atheros code
```

**Key Features:**
- Atheros AR9xxx chipsets
- 802.11a/b/g/n (2.4 GHz and 5 GHz)
- Up to 300 Mbps (2x2 MIMO)
- Hardware encryption (WEP/TKIP/CCMP)
- Beacon generation, power save modes
- Spectrum management (DFS - Dynamic Frequency Selection)

**Data Structures:**
```c
typedef struct {
    pci_device_t* pci_dev;
    void* mmio_base;
    
    // Hardware info
    uint32_t chip_id;
    uint8_t mac_addr[6];
    
    // TX/RX queues
    ath9k_tx_queue_t* tx_queues[10];
    ath9k_rx_queue_t* rx_queue;
    
    // PHY state
    uint32_t current_channel;
    uint32_t channel_flags;
    
    // mac80211
    ieee80211_hw_t* hw;
    
    telemetry_t* metrics;
} ath9k_softc_t;

typedef struct {
    void* desc_ring;         // TX descriptor ring
    void** buffers;
    uint32_t head;
    uint32_t tail;
} ath9k_tx_queue_t;
```

**Driver Flow:**
1. **Probe:** Detect Atheros card via PCI (vendor 0x168C)
2. **Initialize:**
   - Map MMIO registers
   - Reset hardware
   - Load EEPROM (MAC address, calibration data)
   - Initialize PHY
   - Setup TX/RX descriptor rings
   - Register with mac80211
3. **Scan:**
   - Tune to each channel (1-11 for 2.4 GHz)
   - Send probe requests
   - Collect beacon/probe responses
   - Build BSS (Basic Service Set) list
4. **Association:**
   - Send authentication frame (Open System or WPA)
   - Send association request
   - Wait for association response
   - Configure encryption keys
5. **TX/RX:**
   - Wrap data in 802.11 frames
   - Hardware encryption
   - DMA to/from ring buffers
   - Interrupt on completion

**Dependencies:**
- PCI subsystem
- mac80211 (generic 802.11 MAC layer)
- cfg80211 (configuration API)
- Cryptographic library (WPA2/AES)

**Testing Approach:**
- QEMU: Limited WiFi emulation (use virtio-net as fallback)
- Real hardware: Atheros AR9xxx PCIe cards, USB dongles
- Integration tests: Scan, connect to WPA2-PSK network, download file
- Performance tests: iperf3 over WiFi

**AI Integration:**
- Telemetry: Signal strength (RSSI), packet loss, throughput, roaming events
- Adaptive tuning: TX power, rate selection (802.11 rate adaptation)
- Anomaly detection: Deauthentication attacks, channel interference

**Estimated Complexity:** 9/10  
**Lines of Code:** ~6,000  
**Critical Path:** 802.11 MAC layer, hardware initialization, encryption

---

### 6.2 iwlwifi Driver (Intel WiFi)

**Priority:** Medium  
**Complexity:** Very High  
**Estimated Duration:** 10-12 weeks

#### Architecture Design

**Similar to ath9k, but:**
- Intel WiFi chipsets (AC 7260, AX200, etc.)
- Requires signed firmware (iwlwifi-*.ucode)
- 802.11ac Wave 2 (up to 1.7 Gbps)
- 802.11ax (WiFi 6, up to 2.4 Gbps)
- Advanced features (MU-MIMO, beamforming, OFDMA)

**Firmware Loading:**
- Load firmware from `/lib/firmware/iwlwifi-*`
- Send to device via DMA
- Wait for firmware ready event
- Initialize firmware (send INIT commands)

**Estimated Complexity:** 10/10  
**Lines of Code:** ~8,000  
**Critical Path:** Firmware protocol, mac80211 integration

---

## 7. Driver Framework Enhancements

### 7.1 Hot-Plug Support

**Components:**
- PCI hot-plug (PCIe surprise removal)
- USB hot-plug (already in xHCI)
- Sysfs device tree updates
- Udev integration (userspace events)

**Implementation:**
- Poll PCI link status (LTSSM - Link Training Status State Machine)
- Generate device add/remove events
- Call driver probe/remove callbacks
- Cleanup device state on removal

---

### 7.2 Power Management

**ACPI Integration:**
- D0-D3 power states (device power)
- S3 (suspend to RAM), S4 (hibernate)
- Runtime PM (auto-suspend idle devices)
- Wake-on-LAN, Wake-on-USB

**Driver Callbacks:**
```c
struct driver {
    int (*suspend)(device_t* dev);
    int (*resume)(device_t* dev);
    int (*runtime_suspend)(device_t* dev);
    int (*runtime_resume)(device_t* dev);
};
```

---

### 7.3 AI Telemetry Infrastructure

**Per-Driver Metrics:**
```c
typedef struct {
    uint64_t total_operations;
    uint64_t failed_operations;
    uint64_t total_bytes;
    uint64_t avg_latency_us;
    uint64_t max_latency_us;
    uint64_t interrupt_count;
    uint64_t dma_errors;
    uint64_t device_errors;
    // Driver-specific metrics
    void* driver_data;
} telemetry_t;
```

**AI Interface:**
- Syscall: `ai_query_driver_telemetry(driver_name, metric_type)`
- Ring buffer: continuous telemetry stream
- Aggregation: per-driver, per-device, system-wide

**AI Actions:**
- Tune driver parameters (ring sizes, timeouts, thresholds)
- Recommend driver upgrades
- Predict hardware failures (SMART for disks, WiFi signal degradation)

---

## 8. Implementation Timeline

### Phase 2 (Weeks 1-8): Storage & Networking

**Week 1-4: NVMe Driver**
- Week 1: Admin queue, basic I/O
- Week 2: Namespace enumeration, block layer integration
- Week 3: Multi-queue, MSI-X interrupts
- Week 4: Testing, optimization

**Week 5-7: AHCI/SATA Driver**
- Week 5: Port detection, FIS handling
- Week 6: NCQ, error handling
- Week 7: Testing, hot-plug

**Week 8: Network Drivers**
- Week 8a: Intel e1000 (basic TX/RX)
- Week 8b: Realtek 8169 (if time permits)

---

### Phase 3 (Weeks 9-16): Graphics, Audio, USB

**Week 9-14: Intel i915 Graphics**
- Week 9-10: DRM core, GEM
- Week 11-12: Modesetting, display detection
- Week 13: EDID parsing, multi-monitor
- Week 14: Testing, stabilization

**Week 15-16: Intel HDA Audio**
- Week 15: Codec enumeration, stream setup
- Week 16: Playback, testing

**Week 17-22: USB Subsystem**
- Week 17-20: xHCI controller
  - Week 17: Ring management, command submission
  - Week 18: Device enumeration
  - Week 19: Data transfers
  - Week 20: Testing
- Week 21-22: USB HID driver
  - Week 21: Report parsing, keyboard/mouse
  - Week 22: Testing

---

### Phase 3+ (Weeks 23+): Wireless (Optional)

**Week 23-30: Wireless Drivers**
- Week 23-26: mac80211 stack
- Week 27-30: ath9k driver
- Week 31+: iwlwifi (if needed)

---

## 9. Testing Strategy

### Unit Tests
- Driver initialization/cleanup
- Descriptor ring management
- Command submission/completion
- Error handling paths

### Integration Tests
- Boot from NVMe drive
- Network connectivity (DHCP, ping, HTTP)
- Display output (modesetting, multi-monitor)
- Audio playback (sine wave, WAV file)
- USB device enumeration (keyboard, storage)

### Hardware Compatibility Testing
- **Storage:** NVMe (Intel 660p, Samsung 970 EVO), SATA (WD Blue, Seagate)
- **Network:** Intel I210/I350, Realtek RTL8111
- **Graphics:** Intel HD 5500/6000, Iris Plus
- **Audio:** Realtek ALC892, Conexant CX20722
- **USB:** Intel xHCI, ASMedia xHCI
- **Wireless:** Atheros AR9485, Intel AC 7260

### Performance Benchmarks
- **Storage:** fio (sequential/random read/write, IOPS)
- **Network:** iperf3 (TCP/UDP throughput)
- **Graphics:** glxgears, unigine-heaven (once OpenGL works)
- **Audio:** JACK latency test
- **USB:** USB flash drive read/write speed

---

## 10. Dependencies & Prerequisites

### Kernel Infrastructure (Phase 1 Complete)
- ✅ PCI subsystem (enumerate devices)
- ✅ Interrupt handling (IRQ, MSI, MSI-X)
- ✅ DMA allocator (physically contiguous buffers)
- ✅ Memory management (PMM, VMM, heap)
- ✅ Timer (PIT)

### New Infrastructure (Build in Phase 2)
- Block layer (generic block device interface)
- Network stack (Ethernet, IP, TCP/UDP, socket API)
- DRM/KMS (graphics subsystem)
- ALSA (audio subsystem)
- Input subsystem (keyboard/mouse events)
- Sysfs/procfs (device tree, driver info)

---

## 11. Risk Assessment

### High Risks
1. **Intel i915 Complexity:** Extremely complex driver, may take longer than estimated
   - **Mitigation:** Start with basic framebuffer, incremental modesetting
2. **xHCI USB Specification:** TRB handling is error-prone
   - **Mitigation:** Extensive unit tests, reference Linux driver code
3. **Wireless Firmware:** iwlwifi requires signed firmware, legal/licensing issues
   - **Mitigation:** Focus on ath9k first (no firmware required)

### Medium Risks
1. **Hardware Variability:** NVMe vendors have different quirks
   - **Mitigation:** Test on multiple devices, add vendor-specific workarounds
2. **Interrupt Storms:** Poorly tuned drivers can cause system instability
   - **Mitigation:** Interrupt coalescing, rate limiting, watchdog timers
3. **DMA Errors:** Buffer alignment, 32-bit vs 64-bit addressing
   - **Mitigation:** DMA API with validation, IOMMU support (Phase 4)

### Low Risks
1. **Serial/UART Drivers:** Simple, well-documented
2. **PS/2 Keyboard:** Already implemented in Phase 1
3. **AHCI/SATA:** Mature specification, many reference implementations

---

## 12. Success Metrics

### Phase 2 Complete (Storage & Network)
- ✅ Boot from NVMe SSD
- ✅ Boot from SATA HDD
- ✅ Network connectivity (DHCP, ping, TCP)
- ✅ File transfer over network (HTTP download)

### Phase 3 Complete (Graphics, Audio, USB)
- ✅ Graphical desktop on Intel GPU
- ✅ Audio playback through speakers
- ✅ USB keyboard/mouse input
- ✅ USB flash drive read/write

### Phase 3+ (Wireless)
- ✅ WiFi scan and connect
- ✅ WPA2-PSK authentication
- ✅ Internet access over WiFi

---

## 13. Team Allocation

**10-Agent Team:**
- **Agent 1-2:** Storage drivers (NVMe, AHCI)
- **Agent 3:** Network drivers (e1000, r8169)
- **Agent 4-5:** Graphics (i915, DRM core)
- **Agent 6:** Audio (HDA)
- **Agent 7-8:** USB (xHCI, HID)
- **Agent 9:** Wireless (ath9k, mac80211)
- **Agent 10:** Integration, testing, AI telemetry

---

## 14. Conclusion

This driver expansion plan provides a clear roadmap for achieving Linux-level hardware compatibility in AutomationOS. By prioritizing high-impact drivers (NVMe, e1000, i915, xHCI) and following a phased approach, we can deliver a functional system incrementally while managing complexity.

The driver framework is designed with AI observability from the start, with telemetry hooks and tuning interfaces in every driver. This enables the AI service to optimize system behavior, predict failures, and provide intelligent recommendations.

**Estimated Total Effort:** 12-16 weeks with 10 agents  
**Critical Path:** i915 graphics, xHCI USB, NVMe storage  
**High-Risk Items:** i915, xHCI, iwlwifi

With this plan, AutomationOS Phase 2-3 will transform from a minimal shell to a fully functional operating system capable of running on modern hardware with storage, networking, graphics, audio, USB, and wireless support.

---

**End of Driver Expansion Plan**
