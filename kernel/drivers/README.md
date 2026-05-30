# Kernel Drivers

This directory contains low-level hardware drivers for AutomationOS.

## Serial Driver (serial.c)

COM1 serial port driver for debug output and early boot logging.

### Features
- Initializes COM1 port at 0x3F8
- 38400 baud rate, 8N1 (8 bits, no parity, 1 stop bit)
- FIFO enabled with 14-byte threshold
- Busy-wait transmission

### API
```c
void serial_init(void);                       // Initialize COM1
void serial_putchar(char c);                  // Output single character
void serial_write(const char* str, size_t len); // Output string
```

### Usage
```c
serial_init();
serial_write("Hello, World!\n", 14);
```

### Implementation Details
- COM1 base port: 0x3F8
- Line Control Register (LCR): 0x03 (8N1)
- FIFO Control Register (FCR): 0xC7 (FIFO enabled, clear, 14-byte threshold)
- Modem Control Register (MCR): 0x0B (IRQs enabled, RTS/DSR set)
- Divisor: 0x0003 (38400 baud)

### Testing
The driver can be tested in QEMU with:
```bash
qemu-system-x86_64 -serial stdio ...
```
All output to COM1 will appear in the terminal.

## Future Drivers
- ps2.c - PS/2 keyboard driver
- framebuffer.c - VESA/VBE framebuffer driver
- pit.c - Programmable Interval Timer
- virtio.c - Virtio devices for QEMU
