/**
 * AutomationOS - Pseudo-Terminal (PTY) Driver Header
 */

#ifndef PTY_H
#define PTY_H

#include "../../include/types.h"

// PTY API
void pty_init(void);
int pty_allocate(void);
void pty_free(uint32_t index);

// Master side operations
ssize_t pty_master_write(uint32_t index, const uint8_t *data, uint32_t size);
ssize_t pty_master_read(uint32_t index, uint8_t *data, uint32_t size);
uint32_t pty_master_available(uint32_t index);

// Slave side operations
ssize_t pty_slave_write(uint32_t index, const uint8_t *data, uint32_t size);
ssize_t pty_slave_read(uint32_t index, uint8_t *data, uint32_t size);
uint32_t pty_slave_available(uint32_t index);

// Terminal control
int pty_set_winsize(uint32_t index, uint16_t rows, uint16_t cols);
int pty_get_winsize(uint32_t index, uint16_t *rows, uint16_t *cols);
int pty_set_termios(uint32_t index, uint32_t flags);
int pty_get_termios(uint32_t index, uint32_t *flags);
int pty_get_slave_name(uint32_t index, char *name, uint32_t size);

// ioctl commands
#define TIOCGWINSZ  0x5413  // Get window size
#define TIOCSWINSZ  0x5414  // Set window size
#define TCGETS      0x5401  // Get termios
#define TCSETS      0x5402  // Set termios

#endif // PTY_H
