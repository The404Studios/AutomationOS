#ifndef DRIVERS_H
#define DRIVERS_H

#include "types.h"

// Serial driver
void serial_init(void);
void serial_putchar(char c);
void serial_write(const char* str, size_t len);

// PS/2 keyboard driver
void ps2_init(void);
char ps2_getchar(void);

// Framebuffer geometry descriptor — used by framebuffer_get_info() and
// the sys_fb_acquire syscall to export framebuffer info to userspace.
typedef struct {
    uint64_t phys_base;  // physical base address of the linear framebuffer
    uint32_t width;      // width in pixels
    uint32_t height;     // height in pixels
    uint32_t pitch;      // bytes per scanline
    uint32_t bpp;        // bits per pixel
} fb_info_t;

// Framebuffer driver
void framebuffer_init(uint64_t fb_addr, uint32_t width, uint32_t height, uint32_t pitch);
void framebuffer_clear(uint32_t color);
void framebuffer_plot_pixel(uint32_t x, uint32_t y, uint32_t color);
void framebuffer_putchar(char c, uint32_t x, uint32_t y, uint32_t color);
void framebuffer_puts_scaled(const char* s, uint32_t x, uint32_t y, uint32_t color, uint32_t scale);
void framebuffer_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void framebuffer_draw_hline(uint32_t x, uint32_t y, uint32_t length, uint32_t color);
void framebuffer_draw_vline(uint32_t x, uint32_t y, uint32_t length, uint32_t color);
int  framebuffer_get_info(fb_info_t* out);

// Timer driver (PIT - Programmable Interval Timer)
void pit_init(uint32_t frequency);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ticks_ms(void);     // milliseconds since boot
uint32_t timer_get_frequency(void);
void timer_sleep(uint32_t ms);

// NVMe Storage driver
void nvme_init(void);

// HDA Audio driver
void hda_init(void);
void hda_run_tests(void);

// USB subsystem
void usb_init(void);
void usb_hid_init(void);
void usb_hid_test_init(void);
void usb_list_devices(void);
void usb_list_drivers(void);

// Input subsystem
void input_init(void);
void input_list_devices(void);

// Wireless networking (ath9k)
int ath9k_init(void);
void ath9k_exit(void);

#endif
