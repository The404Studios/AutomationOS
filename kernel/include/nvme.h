#ifndef NVME_H
#define NVME_H

#include "types.h"
#include "pci.h"

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

// NVMe Register Offsets (BAR0)
#define NVME_REG_CAP        0x00    // Controller Capabilities
#define NVME_REG_VS         0x08    // Version
#define NVME_REG_INTMS      0x0C    // Interrupt Mask Set
#define NVME_REG_INTMC      0x10    // Interrupt Mask Clear
#define NVME_REG_CC         0x14    // Controller Configuration
#define NVME_REG_CSTS       0x1C    // Controller Status
#define NVME_REG_AQA        0x24    // Admin Queue Attributes
#define NVME_REG_ASQ        0x28    // Admin Submission Queue Base Address
#define NVME_REG_ACQ        0x30    // Admin Completion Queue Base Address

// Controller Configuration (CC) Register Bits
#define NVME_CC_EN          (1 << 0)    // Enable
#define NVME_CC_CSS_NVM     (0 << 4)    // NVM Command Set
#define NVME_CC_MPS(x)      ((x) << 7)  // Memory Page Size (2^(12+x))
#define NVME_CC_AMS_RR      (0 << 11)   // Round Robin
#define NVME_CC_SHN_NONE    (0 << 14)   // No shutdown notification
#define NVME_CC_SHN_NORMAL  (1 << 14)   // Normal shutdown
#define NVME_CC_IOSQES(x)   ((x) << 16) // I/O Submission Queue Entry Size
#define NVME_CC_IOCQES(x)   ((x) << 20) // I/O Completion Queue Entry Size

// Controller Status (CSTS) Register Bits
#define NVME_CSTS_RDY       (1 << 0)    // Ready
#define NVME_CSTS_CFS       (1 << 1)    // Controller Fatal Status
#define NVME_CSTS_SHST_MASK (3 << 2)    // Shutdown Status
#define NVME_CSTS_SHST_NORMAL (0 << 2)
#define NVME_CSTS_SHST_OCCURRING (1 << 2)
#define NVME_CSTS_SHST_COMPLETE (2 << 2)

// Admin Commands
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_ABORT        0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A
#define NVME_ADMIN_ASYNC_EVENT  0x0C
#define NVME_ADMIN_NS_MANAGE    0x0D
#define NVME_ADMIN_NS_ATTACH    0x15
#define NVME_ADMIN_FORMAT_NVM   0x80

// NVM Commands (I/O)
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02
#define NVME_CMD_WRITE_UNCOR    0x04
#define NVME_CMD_COMPARE        0x05
#define NVME_CMD_WRITE_ZEROS    0x08
#define NVME_CMD_DSM            0x09    // Dataset Management (TRIM)

// Identify CNS (Controller or Namespace Structure)
#define NVME_IDENTIFY_NS        0x00
#define NVME_IDENTIFY_CTRL      0x01
#define NVME_IDENTIFY_NS_ACTIVE 0x02
#define NVME_IDENTIFY_NS_LIST   0x10

// Queue Entry Sizes
#define NVME_SQ_ENTRY_SIZE      64
#define NVME_CQ_ENTRY_SIZE      16

// Status Codes
#define NVME_SC_SUCCESS         0x00
#define NVME_SC_INVALID_OPCODE  0x01
#define NVME_SC_INVALID_FIELD   0x02
#define NVME_SC_CMDID_CONFLICT  0x03
#define NVME_SC_DATA_XFER_ERROR 0x04
#define NVME_SC_ABORTED_POWER_LOSS 0x05
#define NVME_SC_INTERNAL_ERROR  0x06
#define NVME_SC_ABORT_REQ       0x07
#define NVME_SC_ABORT_QUEUE     0x08
#define NVME_SC_FUSED_FAIL      0x09
#define NVME_SC_FUSED_MISSING   0x0A
#define NVME_SC_INVALID_NS      0x0B
#define NVME_SC_LBA_RANGE       0x80

// PRP (Physical Region Page) constants
#define NVME_PAGE_SIZE          4096

// NVMe Submission Queue Entry (64 bytes)
typedef struct {
    uint32_t cdw0;          // Command Dword 0 (opcode + flags + CID)
    uint32_t nsid;          // Namespace ID
    uint64_t rsvd1;
    uint64_t metadata;      // Metadata pointer
    uint64_t prp1;          // PRP Entry 1 (physical address)
    uint64_t prp2;          // PRP Entry 2 (physical address or PRP list)
    uint32_t cdw10;         // Command-specific
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} PACKED nvme_command_t;

// NVMe Completion Queue Entry (16 bytes)
typedef struct {
    uint32_t dw0;           // Command-specific result
    uint32_t dw1;           // Reserved
    uint16_t sq_head;       // Submission Queue Head Pointer
    uint16_t sq_id;         // Submission Queue Identifier
    uint16_t cid;           // Command Identifier
    uint16_t status;        // Status (phase bit + status code)
} PACKED nvme_completion_t;

// NVMe Queue Pair
typedef struct {
    volatile nvme_command_t* sq;     // Submission Queue (controller reads these)
    volatile nvme_completion_t* cq;  // Completion Queue (controller writes these)

    volatile uint32_t* sq_doorbell;  // Submission Queue Doorbell (MMIO)
    volatile uint32_t* cq_doorbell;  // Completion Queue Doorbell (MMIO)

    uint16_t sq_tail;       // Submission Queue Tail
    uint16_t cq_head;       // Completion Queue Head
    uint16_t cq_phase;      // Completion Queue Phase Bit

    uint16_t queue_id;      // Queue ID (0 = admin)
    uint16_t queue_depth;   // Number of entries
} nvme_queue_t;

// NVMe Namespace
typedef struct {
    uint32_t nsid;          // Namespace ID
    uint64_t size_blocks;   // Size in blocks
    uint32_t block_size;    // Block size in bytes
    uint64_t capacity;      // Capacity in bytes
    bool active;            // Namespace is active
} nvme_namespace_t;

// NVMe Controller
typedef struct {
    pci_device_t* pci_dev;  // PCI device
    volatile uint8_t* bar;  // BAR0 (controller registers)

    uint64_t cap;           // Controller Capabilities
    uint32_t vs;            // Version
    uint32_t doorbell_stride; // Doorbell stride (in bytes)
    uint16_t max_queue_entries; // Max queue entries

    nvme_queue_t admin_queue; // Admin queue
    nvme_queue_t* io_queues;  // I/O queues
    uint16_t num_io_queues;   // Number of I/O queues

    nvme_namespace_t* namespaces; // Array of namespaces
    uint32_t num_namespaces;      // Number of namespaces

    uint16_t next_cid;      // Next command ID
} nvme_controller_t;

// Identify Controller Data Structure (4096 bytes)
typedef struct {
    uint16_t vendor_id;     // PCI Vendor ID
    uint16_t subsystem_vendor_id;
    char serial_number[20]; // Serial Number
    char model_number[40];  // Model Number
    char firmware_rev[8];   // Firmware Revision
    uint8_t rab;            // Recommended Arbitration Burst
    uint8_t ieee[3];        // IEEE OUI Identifier
    uint8_t cmic;           // Controller Multi-Path I/O
    uint8_t mdts;           // Maximum Data Transfer Size
    uint16_t cntlid;        // Controller ID
    uint32_t ver;           // Version
    uint32_t rtd3r;         // RTD3 Resume Latency
    uint32_t rtd3e;         // RTD3 Entry Latency
    uint32_t oaes;          // Optional Async Events Supported
    uint8_t rsvd1[160];
    uint16_t oacs;          // Optional Admin Command Support
    uint8_t acl;            // Abort Command Limit
    uint8_t aerl;           // Async Event Request Limit
    uint8_t frmw;           // Firmware Updates
    uint8_t lpa;            // Log Page Attributes
    uint8_t elpe;           // Error Log Page Entries
    uint8_t npss;           // Number of Power States Support
    uint8_t avscc;          // Admin Vendor Specific Command
    uint8_t apsta;          // Autonomous Power State Transition
    uint16_t wctemp;        // Warning Composite Temperature
    uint16_t cctemp;        // Critical Composite Temperature
    uint16_t mtfa;          // Maximum Time for Firmware Activation
    uint32_t hmpre;         // Host Memory Buffer Preferred Size
    uint32_t hmmin;         // Host Memory Buffer Minimum Size
    uint8_t rsvd2[216];
    uint8_t sqes;           // Submission Queue Entry Size
    uint8_t cqes;           // Completion Queue Entry Size
    uint16_t rsvd3;
    uint32_t nn;            // Number of Namespaces
    uint16_t oncs;          // Optional NVM Command Support
    uint16_t fuses;         // Fused Operation Support
    uint8_t fna;            // Format NVM Attributes
    uint8_t vwc;            // Volatile Write Cache
    uint16_t awun;          // Atomic Write Unit Normal
    uint16_t awupf;         // Atomic Write Unit Power Fail
    uint8_t nvscc;          // NVM Vendor Specific Command
    uint8_t rsvd4;
    uint16_t acwu;          // Atomic Compare & Write Unit
    uint16_t rsvd5;
    uint32_t sgls;          // SGL Support
    uint8_t rsvd6[3540];
} PACKED nvme_identify_controller_t;

// Identify Namespace Data Structure (4096 bytes)
typedef struct {
    uint64_t nsze;          // Namespace Size
    uint64_t ncap;          // Namespace Capacity
    uint64_t nuse;          // Namespace Utilization
    uint8_t nsfeat;         // Namespace Features
    uint8_t nlbaf;          // Number of LBA Formats
    uint8_t flbas;          // Formatted LBA Size
    uint8_t mc;             // Metadata Capabilities
    uint8_t dpc;            // End-to-end Data Protection Capabilities
    uint8_t dps;            // End-to-end Data Protection Type Settings
    uint8_t nmic;           // Namespace Multi-path I/O
    uint8_t rescap;         // Reservation Capabilities
    uint8_t fpi;            // Format Progress Indicator
    uint8_t rsvd1[2];
    uint16_t nawun;         // Namespace Atomic Write Unit Normal
    uint16_t nawupf;        // Namespace Atomic Write Unit Power Fail
    uint16_t nacwu;         // Namespace Atomic Compare & Write Unit
    uint16_t nabsn;         // Namespace Atomic Boundary Size Normal
    uint16_t nabo;          // Namespace Atomic Boundary Offset
    uint16_t nabspf;        // Namespace Atomic Boundary Size Power Fail
    uint8_t rsvd2[14];
    uint8_t nvmcap[16];     // NVM Capacity
    uint8_t rsvd3[40];
    uint8_t nguid[16];      // Namespace Globally Unique Identifier
    uint64_t eui64;         // IEEE Extended Unique Identifier
    struct {
        uint16_t ms;        // Metadata Size
        uint8_t lbads;      // LBA Data Size (2^n)
        uint8_t rp;         // Relative Performance
    } PACKED lbaf[16];      // LBA Format Support
    uint8_t rsvd4[3776];
} PACKED nvme_identify_namespace_t;

// NVMe Driver Functions
void nvme_init(void);
bool nvme_probe(pci_device_t* pci_dev);
nvme_controller_t* nvme_init_controller(pci_device_t* pci_dev);
void nvme_shutdown_controller(nvme_controller_t* ctrl);

// Admin Commands
int nvme_identify_controller(nvme_controller_t* ctrl, nvme_identify_controller_t* id_ctrl);
int nvme_identify_namespace(nvme_controller_t* ctrl, uint32_t nsid, nvme_identify_namespace_t* id_ns);
int nvme_create_io_cq(nvme_controller_t* ctrl, uint16_t qid, uint16_t qsize, uint64_t phys_addr);
int nvme_create_io_sq(nvme_controller_t* ctrl, uint16_t qid, uint16_t qsize, uint64_t phys_addr, uint16_t cqid);
int nvme_delete_io_sq(nvme_controller_t* ctrl, uint16_t qid);
int nvme_delete_io_cq(nvme_controller_t* ctrl, uint16_t qid);
int nvme_set_features(nvme_controller_t* ctrl, uint8_t fid, uint32_t value);
int nvme_get_features(nvme_controller_t* ctrl, uint8_t fid, uint32_t* value);

// I/O Commands
int nvme_read(nvme_controller_t* ctrl, uint32_t nsid, uint64_t lba, uint16_t count, void* buffer);
int nvme_write(nvme_controller_t* ctrl, uint32_t nsid, uint64_t lba, uint16_t count, const void* buffer);
int nvme_flush(nvme_controller_t* ctrl, uint32_t nsid);
int nvme_write_zeros(nvme_controller_t* ctrl, uint32_t nsid, uint64_t lba, uint16_t count);
int nvme_trim(nvme_controller_t* ctrl, uint32_t nsid, uint64_t lba, uint32_t count);

// Queue Management
void nvme_init_queue(nvme_queue_t* queue, uint16_t queue_id, uint16_t queue_depth);
void nvme_free_queue(nvme_queue_t* queue);
void nvme_submit_command(nvme_queue_t* queue, nvme_command_t* cmd);
int nvme_wait_for_completion(nvme_queue_t* queue, uint16_t cid, nvme_completion_t* completion);
void nvme_process_completions(nvme_queue_t* queue);

// Utility Functions
uint16_t nvme_get_cid(nvme_controller_t* ctrl);
void nvme_build_prp_list(uint64_t phys_addr, size_t size, uint64_t* prp1, uint64_t* prp2);

#endif
