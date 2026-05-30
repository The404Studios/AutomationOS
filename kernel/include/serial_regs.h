#ifndef SERIAL_REGS_H
#define SERIAL_REGS_H

/*
 * Serial Port (UART 16550) Register Definitions
 * ==============================================
 *
 * Defines register offsets and bit flags for 16550 UART serial ports.
 */

/* COM Port Base Addresses */
#define COM1_PORT               0x3F8
#define COM2_PORT               0x2F8
#define COM3_PORT               0x3E8
#define COM4_PORT               0x2E8

/* Register Offsets (from base port) */
#define SERIAL_DATA_REG         0    /* Data register (DLAB=0) */
#define SERIAL_DIVISOR_LOW      0    /* Divisor latch low byte (DLAB=1) */
#define SERIAL_INT_ENABLE       1    /* Interrupt enable register (DLAB=0) */
#define SERIAL_DIVISOR_HIGH     1    /* Divisor latch high byte (DLAB=1) */
#define SERIAL_INT_ID_REG       2    /* Interrupt identification register */
#define SERIAL_FIFO_CTRL        2    /* FIFO control register */
#define SERIAL_LINE_CTRL        3    /* Line control register */
#define SERIAL_MODEM_CTRL       4    /* Modem control register */
#define SERIAL_LINE_STATUS      5    /* Line status register */
#define SERIAL_MODEM_STATUS     6    /* Modem status register */
#define SERIAL_SCRATCH          7    /* Scratch register */

/* Interrupt Enable Register Bits */
#define SERIAL_INT_RECEIVED     0x01    /* Data received interrupt */
#define SERIAL_INT_TRANSMIT     0x02    /* Transmitter empty interrupt */
#define SERIAL_INT_LINE_STATUS  0x04    /* Line status interrupt */
#define SERIAL_INT_MODEM_STATUS 0x08    /* Modem status interrupt */
#define SERIAL_INT_DISABLE_ALL  0x00    /* Disable all interrupts */

/* Line Control Register Bits */
#define SERIAL_LCR_DLAB         0x80    /* Divisor latch access bit */
#define SERIAL_LCR_BREAK        0x40    /* Set break enable */
#define SERIAL_LCR_PARITY_NONE  0x00    /* No parity */
#define SERIAL_LCR_PARITY_ODD   0x08    /* Odd parity */
#define SERIAL_LCR_PARITY_EVEN  0x18    /* Even parity */
#define SERIAL_LCR_STOP_1       0x00    /* 1 stop bit */
#define SERIAL_LCR_STOP_2       0x04    /* 2 stop bits */
#define SERIAL_LCR_BITS_5       0x00    /* 5 data bits */
#define SERIAL_LCR_BITS_6       0x01    /* 6 data bits */
#define SERIAL_LCR_BITS_7       0x02    /* 7 data bits */
#define SERIAL_LCR_BITS_8       0x03    /* 8 data bits */

/* Common Line Control Configurations */
#define SERIAL_LCR_8N1          (SERIAL_LCR_BITS_8 | SERIAL_LCR_PARITY_NONE | \
                                 SERIAL_LCR_STOP_1)  /* 8 bits, no parity, 1 stop */

/* FIFO Control Register Bits */
#define SERIAL_FIFO_ENABLE      0x01    /* Enable FIFO */
#define SERIAL_FIFO_CLEAR_RX    0x02    /* Clear receive FIFO */
#define SERIAL_FIFO_CLEAR_TX    0x04    /* Clear transmit FIFO */
#define SERIAL_FIFO_DMA_MODE    0x08    /* DMA mode select */
#define SERIAL_FIFO_TRIGGER_1   0x00    /* 1-byte trigger level */
#define SERIAL_FIFO_TRIGGER_4   0x40    /* 4-byte trigger level */
#define SERIAL_FIFO_TRIGGER_8   0x80    /* 8-byte trigger level */
#define SERIAL_FIFO_TRIGGER_14  0xC0    /* 14-byte trigger level */

/* Common FIFO Configuration */
#define SERIAL_FIFO_CONFIG      (SERIAL_FIFO_ENABLE | SERIAL_FIFO_CLEAR_RX | \
                                 SERIAL_FIFO_CLEAR_TX | SERIAL_FIFO_TRIGGER_14)  /* 0xC7 */

/* Modem Control Register Bits */
#define SERIAL_MCR_DTR          0x01    /* Data terminal ready */
#define SERIAL_MCR_RTS          0x02    /* Request to send */
#define SERIAL_MCR_OUT1         0x04    /* Output 1 */
#define SERIAL_MCR_OUT2         0x08    /* Output 2 (enable IRQ) */
#define SERIAL_MCR_LOOPBACK     0x10    /* Loopback mode */

/* Common Modem Configuration */
#define SERIAL_MCR_CONFIG       (SERIAL_MCR_DTR | SERIAL_MCR_RTS | \
                                 SERIAL_MCR_OUT2)  /* 0x0B */

/* Line Status Register Bits */
#define SERIAL_LSR_DATA_READY   0x01    /* Data ready */
#define SERIAL_LSR_OVERRUN_ERR  0x02    /* Overrun error */
#define SERIAL_LSR_PARITY_ERR   0x04    /* Parity error */
#define SERIAL_LSR_FRAMING_ERR  0x08    /* Framing error */
#define SERIAL_LSR_BREAK_INT    0x10    /* Break interrupt */
#define SERIAL_LSR_TX_EMPTY     0x20    /* Transmitter holding register empty */
#define SERIAL_LSR_TX_IDLE      0x40    /* Transmitter empty */
#define SERIAL_LSR_FIFO_ERROR   0x80    /* Error in FIFO */

/* Baud Rate Divisors (for 115200 base rate) */
#define SERIAL_BAUD_115200      1       /* Divisor for 115200 baud */
#define SERIAL_BAUD_57600       2       /* Divisor for 57600 baud */
#define SERIAL_BAUD_38400       3       /* Divisor for 38400 baud */
#define SERIAL_BAUD_19200       6       /* Divisor for 19200 baud */
#define SERIAL_BAUD_9600        12      /* Divisor for 9600 baud */
#define SERIAL_BAUD_4800        24      /* Divisor for 4800 baud */
#define SERIAL_BAUD_2400        48      /* Divisor for 2400 baud */

#endif
