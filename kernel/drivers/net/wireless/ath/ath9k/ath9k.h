#ifndef ATH9K_H
#define ATH9K_H

#include "../../../../../include/types.h"
#include "../../../../../include/pci.h"
#include "../../../../../include/mac80211.h"

// Atheros Vendor ID
#define ATH_VENDOR_ID       0x168C

// Atheros Device IDs (AR9xxx series)
#define ATH_DEVICE_AR9280   0x0029
#define ATH_DEVICE_AR9285   0x002A
#define ATH_DEVICE_AR9287   0x002E
#define ATH_DEVICE_AR9380   0x0030
#define ATH_DEVICE_AR9485   0x0032
#define ATH_DEVICE_AR9462   0x0034
#define ATH_DEVICE_AR9565   0x0036

// Register Offsets
#define AR_CR               0x0008  // Command Register
#define AR_RXDP             0x000C  // RX Descriptor Pointer
#define AR_CFG              0x0014  // Configuration
#define AR_IER              0x0024  // Interrupt Enable Register
#define AR_TXCFG            0x0030  // TX Configuration
#define AR_RXCFG            0x0034  // RX Configuration
#define AR_MIBC             0x0040  // MIB Control
#define AR_TOPS             0x0044  // Timeout Prescale
#define AR_RXNPTO           0x0048  // No Frame Received Timeout
#define AR_TXNPTO           0x004C  // No TX Frame Sent Timeout
#define AR_RPGTO            0x0050  // Receive Frame Gap Timeout
#define AR_RPCNT            0x0054  // Receive Frame Count
#define AR_MACMISC          0x0058  // MAC Miscellaneous
#define AR_ISR              0x0080  // Interrupt Status Register
#define AR_PISR             0x0084  // Primary Interrupt Status
#define AR_SISR0            0x0088  // Secondary Interrupt Status 0
#define AR_SISR1            0x008C  // Secondary Interrupt Status 1
#define AR_SISR2            0x0090  // Secondary Interrupt Status 2
#define AR_SISR3            0x0094  // Secondary Interrupt Status 3
#define AR_SISR4            0x0098  // Secondary Interrupt Status 4
#define AR_IMR              0x00A0  // Interrupt Mask Register
#define AR_PIMR             0x00A4  // Primary Interrupt Mask
#define AR_SIMR0            0x00A8  // Secondary Interrupt Mask 0
#define AR_SIMR1            0x00AC  // Secondary Interrupt Mask 1
#define AR_SIMR2            0x00B0  // Secondary Interrupt Mask 2
#define AR_SIMR3            0x00B4  // Secondary Interrupt Mask 3
#define AR_SIMR4            0x00B8  // Secondary Interrupt Mask 4

// Command Register Bits
#define AR_CR_RXE           0x00000004  // RX Enable
#define AR_CR_RXD           0x00000020  // RX Disable
#define AR_CR_SWI           0x00000040  // Software Interrupt

// Interrupt Flags
#define AR_ISR_RXOK         0x00000001  // RX OK
#define AR_ISR_RXDESC       0x00000002  // RX Descriptor Request
#define AR_ISR_RXERR        0x00000004  // RX Error
#define AR_ISR_RXNOPKT      0x00000008  // No RX Packet
#define AR_ISR_RXEOL        0x00000010  // RX EOL (End Of List)
#define AR_ISR_RXORN        0x00000020  // RX Overrun
#define AR_ISR_TXOK         0x00000040  // TX OK
#define AR_ISR_TXDESC       0x00000080  // TX Descriptor Request
#define AR_ISR_TXERR        0x00000100  // TX Error
#define AR_ISR_TXNOPKT      0x00000200  // No TX Packet
#define AR_ISR_TXEOL        0x00000400  // TX EOL
#define AR_ISR_TXURN        0x00000800  // TX Underrun
#define AR_ISR_MIB          0x00001000  // MIB Interrupt
#define AR_ISR_SWI          0x00002000  // Software Interrupt
#define AR_ISR_RXPHY        0x00004000  // RX PHY Error
#define AR_ISR_RXKCM        0x00008000  // RX Key Cache Miss
#define AR_ISR_SWBA         0x00010000  // Software Beacon Alert
#define AR_ISR_BRSSI        0x00020000  // Beacon RSSI
#define AR_ISR_BMISS        0x00040000  // Beacon Miss

// RX/TX Descriptor Defines
#define ATH_TXBUF           256
#define ATH_RXBUF           512
#define ATH_TXDESC          400
#define ATH_RXDESC          128

// Buffer sizes
#define ATH_RX_BUF_SIZE     2048
#define ATH_TX_BUF_SIZE     2048

// EEPROM Offsets
#define AR_EEPROM_MAC_ADDR  0x1D  // MAC address location

// Hardware Descriptor
typedef struct {
    uint32_t ds_link;       // Physical address of next descriptor
    uint32_t ds_data;       // Physical address of data buffer
    uint32_t ds_ctl0;       // Control word 0
    uint32_t ds_ctl1;       // Control word 1
    uint32_t ds_hw0;        // Hardware word 0
    uint32_t ds_hw1;        // Hardware word 1
    uint32_t ds_hw2;        // Hardware word 2
    uint32_t ds_hw3;        // Hardware word 3
} PACKED ath9k_desc_t;

// TX Descriptor Control Bits
#define ATH9K_TXDESC_CLRDMASK   0x0001
#define ATH9K_TXDESC_NOACK      0x0002
#define ATH9K_TXDESC_RTSENA     0x0004
#define ATH9K_TXDESC_CTSENA     0x0008
#define ATH9K_TXDESC_INTREQ     0x0010
#define ATH9K_TXDESC_VEOL       0x0020

// RX Descriptor Status Bits
#define ATH9K_RXDESC_DONE       0x00000001
#define ATH9K_RXDESC_ERR_CRC    0x00000002
#define ATH9K_RXDESC_ERR_PHY    0x00000004
#define ATH9K_RXDESC_ERR_MIC    0x00000008

// TX Queue
typedef struct {
    ath9k_desc_t* desc_ring;    // Descriptor ring (DMA)
    uint64_t desc_ring_phys;    // Physical address
    void** buffers;             // Buffer pointers
    uint16_t head;
    uint16_t tail;
    uint16_t size;
    uint8_t qnum;
} ath9k_tx_queue_t;

// RX Queue
typedef struct {
    ath9k_desc_t* desc_ring;    // Descriptor ring (DMA)
    uint64_t desc_ring_phys;    // Physical address
    void** buffers;             // Buffer pointers
    uint16_t head;
    uint16_t size;
} ath9k_rx_queue_t;

// Hardware State
typedef struct {
    pci_device_t* pci_dev;
    void* mem;                  // MMIO base address

    // MAC address
    uint8_t mac_addr[6];

    // Chip info
    uint32_t chip_id;
    uint32_t chip_rev;

    // Queues
    ath9k_tx_queue_t* tx_queues[10];
    ath9k_rx_queue_t* rx_queue;

    // Current channel
    uint32_t curchan;
    uint32_t curchan_flags;

    // Calibration data
    int16_t noise_floor;
    uint8_t tx_power;

    // PHY state
    bool phy_enabled;

    // Statistics
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_errors;
    uint64_t rx_errors;
} ath9k_hw_t;

// Main driver structure
typedef struct {
    ieee80211_hw_t* hw;
    ath9k_hw_t* ah;

    // PCI device
    pci_device_t* pci_dev;

    // IRQ
    uint8_t irq;

    // Interrupt stats
    uint32_t irq_count;
    uint32_t rx_irq_count;
    uint32_t tx_irq_count;

    // Debug
    bool debug;
} ath9k_softc_t;

// Hardware functions
int ath9k_hw_init(ath9k_hw_t* ah, pci_device_t* pci_dev);
void ath9k_hw_deinit(ath9k_hw_t* ah);
int ath9k_hw_reset(ath9k_hw_t* ah);
int ath9k_hw_start_pcicfg(ath9k_hw_t* ah);
void ath9k_hw_read_eeprom(ath9k_hw_t* ah);

// Register access
uint32_t ath9k_hw_reg_read(ath9k_hw_t* ah, uint32_t reg);
void ath9k_hw_reg_write(ath9k_hw_t* ah, uint32_t reg, uint32_t val);

// PHY functions
int ath9k_hw_phy_init(ath9k_hw_t* ah);
int ath9k_hw_set_channel(ath9k_hw_t* ah, uint32_t freq);
void ath9k_hw_set_txpower(ath9k_hw_t* ah, uint8_t power);

// Calibration
void ath9k_hw_calibrate(ath9k_hw_t* ah);

// TX/RX functions
int ath9k_tx_init(ath9k_softc_t* sc);
void ath9k_tx_deinit(ath9k_softc_t* sc);
int ath9k_tx_queue_frame(ath9k_softc_t* sc, uint8_t qnum, sk_buff_t* skb);
void ath9k_tx_tasklet(ath9k_softc_t* sc);

int ath9k_rx_init(ath9k_softc_t* sc);
void ath9k_rx_deinit(ath9k_softc_t* sc);
void ath9k_rx_tasklet(ath9k_softc_t* sc);

// Interrupt handling
void ath9k_irq_handler(void);
void ath9k_tasklet_schedule(ath9k_softc_t* sc);

// Driver functions
int ath9k_init(void);
void ath9k_exit(void);

#endif
