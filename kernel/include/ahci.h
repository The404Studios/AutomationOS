#ifndef AHCI_H
#define AHCI_H

#include "types.h"
#include "pci.h"

// AHCI PCI Class Codes
#define PCI_CLASS_STORAGE_AHCI    0x01
#define PCI_SUBCLASS_AHCI         0x06
#define PCI_PROG_IF_AHCI          0x01

// AHCI Generic Host Control (GHC) Register Offsets
#define AHCI_GHC_CAP              0x00  // Host Capabilities
#define AHCI_GHC_GHC              0x04  // Global Host Control
#define AHCI_GHC_IS               0x08  // Interrupt Status
#define AHCI_GHC_PI               0x0C  // Ports Implemented
#define AHCI_GHC_VS               0x10  // Version
#define AHCI_GHC_CCC_CTL          0x14  // Command Completion Coalescing Control
#define AHCI_GHC_CCC_PORTS        0x18  // Command Completion Coalescing Ports
#define AHCI_GHC_EM_LOC           0x1C  // Enclosure Management Location
#define AHCI_GHC_EM_CTL           0x20  // Enclosure Management Control
#define AHCI_GHC_CAP2             0x24  // Host Capabilities Extended
#define AHCI_GHC_BOHC             0x28  // BIOS/OS Handoff Control and Status

// AHCI Port Register Offsets (relative to port base)
#define AHCI_PORT_CLB             0x00  // Command List Base Address
#define AHCI_PORT_CLBU            0x04  // Command List Base Address Upper
#define AHCI_PORT_FB              0x08  // FIS Base Address
#define AHCI_PORT_FBU             0x0C  // FIS Base Address Upper
#define AHCI_PORT_IS              0x10  // Interrupt Status
#define AHCI_PORT_IE              0x14  // Interrupt Enable
#define AHCI_PORT_CMD             0x18  // Command and Status
#define AHCI_PORT_TFD             0x20  // Task File Data
#define AHCI_PORT_SIG             0x24  // Signature
#define AHCI_PORT_SSTS            0x28  // Serial ATA Status (SCR0: SStatus)
#define AHCI_PORT_SCTL            0x2C  // Serial ATA Control (SCR2: SControl)
#define AHCI_PORT_SERR            0x30  // Serial ATA Error (SCR1: SError)
#define AHCI_PORT_SACT            0x34  // Serial ATA Active (SCR3: SActive)
#define AHCI_PORT_CI              0x38  // Command Issue
#define AHCI_PORT_SNTF            0x3C  // Serial ATA Notification (SCR4)

// AHCI HBA Capabilities (CAP)
#define AHCI_CAP_S64A             (1 << 31)  // Supports 64-bit Addressing
#define AHCI_CAP_SNCQ             (1 << 30)  // Supports Native Command Queuing
#define AHCI_CAP_SSNTF            (1 << 29)  // Supports SNotification Register
#define AHCI_CAP_SMPS             (1 << 28)  // Supports Mechanical Presence Switch
#define AHCI_CAP_SSS              (1 << 27)  // Supports Staggered Spin-up
#define AHCI_CAP_SALP             (1 << 26)  // Supports Aggressive Link Power Management
#define AHCI_CAP_SAL              (1 << 25)  // Supports Activity LED
#define AHCI_CAP_SCLO             (1 << 24)  // Supports Command List Override
#define AHCI_CAP_ISS_MASK         0x00F00000  // Interface Speed Support
#define AHCI_CAP_SAM              (1 << 18)  // Supports AHCI mode only
#define AHCI_CAP_SPM              (1 << 17)  // Supports Port Multiplier
#define AHCI_CAP_FBSS             (1 << 16)  // FIS-based Switching Supported
#define AHCI_CAP_PMD              (1 << 15)  // PIO Multiple DRQ Block
#define AHCI_CAP_SSC              (1 << 14)  // Slumber State Capable
#define AHCI_CAP_PSC              (1 << 13)  // Partial State Capable
#define AHCI_CAP_NCS_MASK         0x00001F00  // Number of Command Slots
#define AHCI_CAP_CCCS             (1 << 7)   // Command Completion Coalescing Supported
#define AHCI_CAP_EMS              (1 << 6)   // Enclosure Management Supported
#define AHCI_CAP_SXS              (1 << 5)   // Supports External SATA
#define AHCI_CAP_NP_MASK          0x0000001F  // Number of Ports

// AHCI Global Host Control (GHC)
#define AHCI_GHC_AE               (1 << 31)  // AHCI Enable
#define AHCI_GHC_MRSM             (1 << 2)   // MSI Revert to Single Message
#define AHCI_GHC_IE               (1 << 1)   // Interrupt Enable
#define AHCI_GHC_HR               (1 << 0)   // HBA Reset

// AHCI Port Command and Status (PxCMD)
#define AHCI_PORT_CMD_ICC_ACTIVE  (1 << 28)  // Interface Communication Control
#define AHCI_PORT_CMD_ASP         (1 << 27)  // Aggressive Slumber/Partial
#define AHCI_PORT_CMD_ALPE        (1 << 26)  // Aggressive Link Power Management Enable
#define AHCI_PORT_CMD_DLAE        (1 << 25)  // Drive LED on ATAPI Enable
#define AHCI_PORT_CMD_ATAPI       (1 << 24)  // Device is ATAPI
#define AHCI_PORT_CMD_APSTE       (1 << 23)  // Automatic Partial to Slumber Transitions Enabled
#define AHCI_PORT_CMD_FBSCP       (1 << 22)  // FIS-based Switching Capable Port
#define AHCI_PORT_CMD_ESP         (1 << 21)  // External SATA Port
#define AHCI_PORT_CMD_CPD         (1 << 20)  // Cold Presence Detection
#define AHCI_PORT_CMD_MPSP        (1 << 19)  // Mechanical Presence Switch Attached to Port
#define AHCI_PORT_CMD_HPCP        (1 << 18)  // Hot Plug Capable Port
#define AHCI_PORT_CMD_PMA         (1 << 17)  // Port Multiplier Attached
#define AHCI_PORT_CMD_CPS         (1 << 16)  // Cold Presence State
#define AHCI_PORT_CMD_CR          (1 << 15)  // Command List Running
#define AHCI_PORT_CMD_FR          (1 << 14)  // FIS Receive Running
#define AHCI_PORT_CMD_MPSS        (1 << 13)  // Mechanical Presence Switch State
#define AHCI_PORT_CMD_CCS_MASK    0x00001F00  // Current Command Slot
#define AHCI_PORT_CMD_FRE         (1 << 4)   // FIS Receive Enable
#define AHCI_PORT_CMD_CLO         (1 << 3)   // Command List Override
#define AHCI_PORT_CMD_POD         (1 << 2)   // Power On Device
#define AHCI_PORT_CMD_SUD         (1 << 1)   // Spin-Up Device
#define AHCI_PORT_CMD_ST          (1 << 0)   // Start

// AHCI Port Task File Data (PxTFD)
#define AHCI_PORT_TFD_ERR_MASK    0xFF00     // Error
#define AHCI_PORT_TFD_STS_MASK    0x00FF     // Status
#define AHCI_PORT_TFD_STS_BSY     (1 << 7)   // Busy
#define AHCI_PORT_TFD_STS_DRQ     (1 << 3)   // Data Transfer Requested
#define AHCI_PORT_TFD_STS_ERR     (1 << 0)   // Error

// AHCI Port SATA Status (PxSSTS)
#define AHCI_PORT_SSTS_IPM_MASK   0x00000F00  // Interface Power Management
#define AHCI_PORT_SSTS_SPD_MASK   0x000000F0  // Current Interface Speed
#define AHCI_PORT_SSTS_DET_MASK   0x0000000F  // Device Detection
#define AHCI_PORT_SSTS_DET_PRESENT 0x00000003  // Device present and Phy communication established

// AHCI Port Interrupt Status/Enable (PxIS/PxIE)
#define AHCI_PORT_INT_CPDS        (1 << 31)  // Cold Port Detect Status
#define AHCI_PORT_INT_TFES        (1 << 30)  // Task File Error Status
#define AHCI_PORT_INT_HBFS        (1 << 29)  // Host Bus Fatal Error Status
#define AHCI_PORT_INT_HBDS        (1 << 28)  // Host Bus Data Error Status
#define AHCI_PORT_INT_IFS         (1 << 27)  // Interface Fatal Error Status
#define AHCI_PORT_INT_INFS        (1 << 26)  // Interface Non-fatal Error Status
#define AHCI_PORT_INT_OFS         (1 << 24)  // Overflow Status
#define AHCI_PORT_INT_IPMS        (1 << 23)  // Incorrect Port Multiplier Status
#define AHCI_PORT_INT_PRCS        (1 << 22)  // PhyRdy Change Status
#define AHCI_PORT_INT_DMPS        (1 << 7)   // Device Mechanical Presence Status
#define AHCI_PORT_INT_PCS         (1 << 6)   // Port Connect Change Status
#define AHCI_PORT_INT_DPS         (1 << 5)   // Descriptor Processed
#define AHCI_PORT_INT_UFI         (1 << 4)   // Unknown FIS Interrupt
#define AHCI_PORT_INT_SDBS        (1 << 3)   // Set Device Bits Interrupt
#define AHCI_PORT_INT_DSS         (1 << 2)   // DMA Setup FIS Interrupt
#define AHCI_PORT_INT_PSS         (1 << 1)   // PIO Setup FIS Interrupt
#define AHCI_PORT_INT_DHRS        (1 << 0)   // Device to Host Register FIS Interrupt

// AHCI FIS Types
#define AHCI_FIS_TYPE_REG_H2D     0x27  // Register FIS - host to device
#define AHCI_FIS_TYPE_REG_D2H     0x34  // Register FIS - device to host
#define AHCI_FIS_TYPE_DMA_ACT     0x39  // DMA activate FIS - device to host
#define AHCI_FIS_TYPE_DMA_SETUP   0x41  // DMA setup FIS - bidirectional
#define AHCI_FIS_TYPE_DATA        0x46  // Data FIS - bidirectional
#define AHCI_FIS_TYPE_BIST        0x58  // BIST activate FIS - bidirectional
#define AHCI_FIS_TYPE_PIO_SETUP   0x5F  // PIO setup FIS - device to host
#define AHCI_FIS_TYPE_DEV_BITS    0xA1  // Set device bits FIS - device to host

// ATA Commands
#define ATA_CMD_READ_DMA_EXT      0x25  // READ DMA EXT
#define ATA_CMD_WRITE_DMA_EXT     0x35  // WRITE DMA EXT
#define ATA_CMD_READ_FPDMA_QUEUED 0x60  // READ FPDMA QUEUED (NCQ)
#define ATA_CMD_WRITE_FPDMA_QUEUED 0x61 // WRITE FPDMA QUEUED (NCQ)
#define ATA_CMD_FLUSH_CACHE       0xE7  // FLUSH CACHE
#define ATA_CMD_FLUSH_CACHE_EXT   0xEA  // FLUSH CACHE EXT
#define ATA_CMD_IDENTIFY_DEVICE   0xEC  // IDENTIFY DEVICE
#define ATA_CMD_SET_FEATURES      0xEF  // SET FEATURES
#define ATA_CMD_SMART             0xB0  // SMART

// ATA Device Signatures
#define ATA_SIG_ATA               0x00000101  // SATA drive
#define ATA_SIG_ATAPI             0xEB140101  // SATAPI drive
#define ATA_SIG_SEMB              0xC33C0101  // Enclosure management bridge
#define ATA_SIG_PM                0x96690101  // Port multiplier

// Command Header Flags
#define AHCI_CMD_HEADER_WRITE     (1 << 6)   // Write (H2D)
#define AHCI_CMD_HEADER_ATAPI     (1 << 5)   // ATAPI
#define AHCI_CMD_HEADER_PREFETCH  (1 << 7)   // Prefetchable
#define AHCI_CMD_HEADER_RESET     (1 << 8)   // Reset
#define AHCI_CMD_HEADER_BIST      (1 << 9)   // BIST
#define AHCI_CMD_HEADER_CLR_BUSY  (1 << 10)  // Clear Busy upon R_OK

// Maximum values
#define AHCI_MAX_PORTS            32
#define AHCI_MAX_CMD_SLOTS        32
#define AHCI_MAX_PRDT_ENTRIES     65536

// Memory sizes
#define AHCI_CMD_LIST_SIZE        1024      // 32 entries * 32 bytes
#define AHCI_RX_FIS_SIZE          256       // Received FIS structure
#define AHCI_CMD_TABLE_SIZE       128       // Command table header
#define AHCI_PRDT_ENTRY_SIZE      16        // PRD table entry

// Timeouts
#define AHCI_TIMEOUT_MS           1000      // 1 second
#define AHCI_SPINUP_TIMEOUT_MS    10000     // 10 seconds

// AHCI HBA Memory Registers
typedef volatile struct {
    // Generic Host Control
    uint32_t cap;           // 0x00: Host Capabilities
    uint32_t ghc;           // 0x04: Global Host Control
    uint32_t is;            // 0x08: Interrupt Status
    uint32_t pi;            // 0x0C: Ports Implemented
    uint32_t vs;            // 0x10: Version
    uint32_t ccc_ctl;       // 0x14: Command Completion Coalescing Control
    uint32_t ccc_ports;     // 0x18: Command Completion Coalescing Ports
    uint32_t em_loc;        // 0x1C: Enclosure Management Location
    uint32_t em_ctl;        // 0x20: Enclosure Management Control
    uint32_t cap2;          // 0x24: Host Capabilities Extended
    uint32_t bohc;          // 0x28: BIOS/OS Handoff Control and Status
    uint8_t reserved[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
} ahci_hba_mem_t;  // NOT packed: MMIO regs are naturally 4-byte aligned with
                   // explicit reserved padding; avoids unaligned-access UB when
                   // taking the address of a register field.

// AHCI Port Registers
typedef volatile struct {
    uint32_t clb;           // 0x00: Command List Base Address
    uint32_t clbu;          // 0x04: Command List Base Address Upper
    uint32_t fb;            // 0x08: FIS Base Address
    uint32_t fbu;           // 0x0C: FIS Base Address Upper
    uint32_t is;            // 0x10: Interrupt Status
    uint32_t ie;            // 0x14: Interrupt Enable
    uint32_t cmd;           // 0x18: Command and Status
    uint32_t reserved0;     // 0x1C
    uint32_t tfd;           // 0x20: Task File Data
    uint32_t sig;           // 0x24: Signature
    uint32_t ssts;          // 0x28: SATA Status (SCR0: SStatus)
    uint32_t sctl;          // 0x2C: SATA Control (SCR2: SControl)
    uint32_t serr;          // 0x30: SATA Error (SCR1: SError)
    uint32_t sact;          // 0x34: SATA Active (SCR3: SActive)
    uint32_t ci;            // 0x38: Command Issue
    uint32_t sntf;          // 0x3C: SATA Notification (SCR4)
    uint32_t fbs;           // 0x40: FIS-based Switching Control
    uint32_t reserved1[11]; // 0x44-0x6F
    uint32_t vendor[4];     // 0x70-0x7F
} ahci_port_regs_t;  // NOT packed: see ahci_hba_mem_t note above.

// Register FIS - Host to Device
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_REG_H2D
    uint8_t pm_port:4;      // Port multiplier
    uint8_t reserved0:3;
    uint8_t c:1;            // 1: Command, 0: Control
    uint8_t command;        // ATA command register
    uint8_t featurel;       // Feature register, 7:0

    uint8_t lba0;           // LBA low register, 7:0
    uint8_t lba1;           // LBA mid register, 15:8
    uint8_t lba2;           // LBA high register, 23:16
    uint8_t device;         // Device register

    uint8_t lba3;           // LBA register, 31:24
    uint8_t lba4;           // LBA register, 39:32
    uint8_t lba5;           // LBA register, 47:40
    uint8_t featureh;       // Feature register, 15:8

    uint8_t countl;         // Count register, 7:0
    uint8_t counth;         // Count register, 15:8
    uint8_t icc;            // Isochronous command completion
    uint8_t control;        // Control register

    uint8_t reserved1[4];
} __attribute__((packed)) fis_reg_h2d_t;

// Register FIS - Device to Host
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_REG_D2H
    uint8_t pm_port:4;      // Port multiplier
    uint8_t reserved0:2;
    uint8_t i:1;            // Interrupt bit
    uint8_t reserved1:1;
    uint8_t status;         // Status register
    uint8_t error;          // Error register

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved2;

    uint8_t countl;
    uint8_t counth;
    uint8_t reserved3[2];

    uint8_t reserved4[4];
} __attribute__((packed)) fis_reg_d2h_t;

// DMA Setup FIS
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_DMA_SETUP
    uint8_t pm_port:4;
    uint8_t reserved0:1;
    uint8_t d:1;            // Data transfer direction: 1 = device to host
    uint8_t i:1;            // Interrupt bit
    uint8_t a:1;            // Auto-activate
    uint8_t reserved1[2];

    uint64_t dma_buffer_id; // DMA Buffer Identifier
    uint32_t reserved2;
    uint32_t dma_buffer_offset;
    uint32_t transfer_count;
    uint32_t reserved3;
} __attribute__((packed)) fis_dma_setup_t;

// PIO Setup FIS
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_PIO_SETUP
    uint8_t pm_port:4;
    uint8_t reserved0:1;
    uint8_t d:1;            // Data transfer direction
    uint8_t i:1;            // Interrupt bit
    uint8_t reserved1:1;
    uint8_t status;
    uint8_t error;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved2;

    uint8_t countl;
    uint8_t counth;
    uint8_t reserved3;
    uint8_t e_status;       // New value of status register

    uint16_t tc;            // Transfer count
    uint8_t reserved4[2];
} __attribute__((packed)) fis_pio_setup_t;

// Set Device Bits FIS
typedef struct {
    uint8_t fis_type;       // FIS_TYPE_DEV_BITS
    uint8_t pm_port:4;
    uint8_t reserved0:2;
    uint8_t i:1;            // Interrupt bit
    uint8_t n:1;            // Notification bit
    uint8_t status_lo:3;
    uint8_t reserved1:1;
    uint8_t status_hi:3;
    uint8_t reserved2:1;
    uint8_t error;
    uint32_t protocol_specific;
} __attribute__((packed)) fis_dev_bits_t;

// Received FIS structure
typedef volatile struct {
    fis_dma_setup_t dsfis;  // DMA Setup FIS
    uint8_t pad0[4];
    fis_pio_setup_t psfis;  // PIO Setup FIS
    uint8_t pad1[12];
    fis_reg_d2h_t rfis;     // Register - Device to Host FIS
    uint8_t pad2[4];
    fis_dev_bits_t sdbfis;  // Set Device Bits FIS
    uint8_t ufis[64];       // Unknown FIS
    uint8_t reserved[96];
} __attribute__((packed)) ahci_rx_fis_t;

// Physical Region Descriptor Table Entry
typedef struct {
    uint64_t dba;           // Data Base Address
    uint32_t reserved;
    uint32_t dbc:22;        // Data Byte Count (0-based, max 4MB)
    uint32_t reserved2:9;
    uint32_t i:1;           // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;

// Command Table
// volatile: the HBA writes the CFIS fields and PRDT byte counts back after completion.
typedef volatile struct {
    uint8_t cfis[64];       // Command FIS
    uint8_t acmd[16];       // ATAPI command
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1]; // Physical Region Descriptor Table (variable length)
} __attribute__((packed)) ahci_cmd_table_t;

// Command Header
// volatile: the HBA DMA-writes prdbc (byte count) back into this struct on completion.
typedef volatile struct {
    uint8_t cfl:5;          // Command FIS length in DWORDS
    uint8_t a:1;            // ATAPI
    uint8_t w:1;            // Write (1: H2D, 0: D2H)
    uint8_t p:1;            // Prefetchable
    uint8_t r:1;            // Reset
    uint8_t b:1;            // BIST
    uint8_t c:1;            // Clear busy upon R_OK
    uint8_t reserved0:1;
    uint8_t pmp:4;          // Port multiplier port
    uint16_t prdtl;         // Physical region descriptor table length
    uint32_t prdbc;         // Physical region descriptor byte count
    uint64_t ctba;          // Command table descriptor base address (128-byte aligned)
    uint32_t reserved1[4];
} __attribute__((packed)) ahci_cmd_header_t;

// AHCI Port Structure
//
// NOTE on DMA addressing: cmd_list / rx_fis / cmd_tables are allocated from the
// PMM (pmm_alloc_page) which returns *physical* pages. Physical RAM 0..N GiB is
// identity-mapped (phys == virt) by paging_init, so these pointers are BOTH a
// valid CPU virtual address (we dereference them directly) AND a valid physical
// DMA address the HBA can use. Do NOT use kmalloc() here: the kernel heap lives
// at a high-half virtual address (0xFFFFFFFF9...) that is meaningless to the
// DMA engine.
typedef struct {
    ahci_port_regs_t* regs;
    ahci_cmd_header_t* cmd_list;   // identity-mapped: vaddr == phys
    ahci_rx_fis_t* rx_fis;         // identity-mapped: vaddr == phys
    ahci_cmd_table_t* cmd_tables[AHCI_MAX_CMD_SLOTS];
    void* dma_bounce;              // one identity-mapped page for DMA bounce I/O

    uint8_t port_num;
    bool device_present;
    bool is_atapi;
    uint32_t device_signature;

    // Device identification
    uint64_t sectors;
    uint32_t sector_size;
    char model[41];
    char serial[21];
    char firmware[9];

    // NCQ support
    bool supports_ncq;
    uint32_t queue_depth;
    uint32_t active_commands;

    // Command slot allocation
    uint32_t slot_bitmap;

    // Error tracking
    uint32_t error_count;
    uint32_t last_error;
} ahci_port_t;

// AHCI Controller Structure
typedef struct {
    pci_device_t* pci_dev;
    ahci_hba_mem_t* abar;

    // Controller capabilities
    uint32_t num_ports;
    uint32_t num_cmd_slots;
    bool supports_64bit;
    bool supports_ncq;
    bool supports_pm;
    uint32_t interface_speed;

    // Port structures
    ahci_port_t ports[AHCI_MAX_PORTS];
    uint32_t ports_implemented;

    // Statistics
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_errors;
} ahci_controller_t;

// Block device interface
typedef struct {
    ahci_port_t* port;
    uint64_t sector_count;
    uint32_t sector_size;
} ahci_block_device_t;

/*
 * Simple block-device API (single global SATA disk == device 0).
 *
 *   ahci_init()    - detect controller, bring up first SATA port, register it.
 *                    Returns 0 on success, negative on failure.
 *   ahci_present() - returns 1 if a usable SATA drive was found, 0 otherwise.
 *   ahci_read()    - read `count` 512-byte sectors at `lba` into `buf`.
 *                    Returns 0 on success, negative on failure.
 *   ahci_write()   - write `count` 512-byte sectors at `lba` from `buf`.
 *                    Returns 0 on success, negative on failure.
 *   blk_read()     - same as ahci_read() but addressed by device number (0).
 *   blk_write()    - same as ahci_write() but addressed by device number (0).
 *   blk_get_port() - get the underlying ahci_port_t for device `dev` (or NULL).
 *
 * `buf` may be any kernel pointer; reads/writes are bounced through an
 * identity-mapped DMA page one sector at a time so callers need not worry
 * about the buffer's physical addressability.
 *
 * Syscall integration (SYS_BLK_READ=49, SYS_BLK_WRITE=50):
 *   Both syscalls take (lba: uint64_t, count: uint32_t, buf: void*).
 *   The handler must validate the user buffer with copy_from_user /
 *   copy_to_user before calling ahci_read / ahci_write.  See the
 *   integration notes at the bottom of ahci.c for the full handler bodies.
 */
int  ahci_init(void);
int  ahci_present(void);
int  ahci_read(uint64_t lba, uint32_t count, void* buf);
int  ahci_write(uint64_t lba, uint32_t count, const void* buf);
int  blk_read(uint32_t dev, uint64_t lba, uint32_t count, void* buf);
int  blk_write(uint32_t dev, uint64_t lba, uint32_t count, const void* buf);
ahci_port_t* blk_get_port(uint32_t dev);

ahci_controller_t* ahci_probe_controller(pci_device_t* pci_dev);
bool ahci_init_controller(ahci_controller_t* controller);
bool ahci_init_port(ahci_controller_t* controller, uint8_t port_num);
bool ahci_port_detect_device(ahci_port_t* port);
bool ahci_port_identify(ahci_port_t* port);
bool ahci_port_start_cmd(ahci_port_t* port);
bool ahci_port_stop_cmd(ahci_port_t* port);
int ahci_port_alloc_slot(ahci_port_t* port);
void ahci_port_free_slot(ahci_port_t* port, int slot);
bool ahci_port_issue_cmd(ahci_port_t* port, int slot);
bool ahci_port_wait_cmd(ahci_port_t* port, int slot, uint32_t timeout_ms);
bool ahci_read_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);
bool ahci_read_sectors_ncq(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);
bool ahci_write_sectors_ncq(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);
bool ahci_flush_cache(ahci_port_t* port);
void ahci_port_handle_interrupt(ahci_port_t* port);
void ahci_handle_interrupt(ahci_controller_t* controller);

#endif
