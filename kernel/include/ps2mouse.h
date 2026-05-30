#ifndef PS2MOUSE_H
#define PS2MOUSE_H

#include "types.h"

/*
 * PS/2 Mouse Driver (standalone, dedicated)
 *
 * Provides robust 3/4-byte packet collection with Intellimouse scroll-wheel
 * support (4-byte packets when device ID == 3 after the magic sample-rate
 * sequence).  Motion and button events are fed into the input/evdev pipeline
 * exactly as the compositor expects:
 *
 *   REL_X / REL_Y  via input_report_rel()
 *   REL_WHEEL      via input_report_rel()   (Intellimouse only)
 *   BTN_LEFT/RIGHT/MIDDLE via input_report_key()
 *
 * Integration
 * -----------
 * Call ps2mouse_init() AFTER input_init(), dev_input_init(), and ps2_init()
 * (keyboard side).  In kernel/kernel.c, just after the existing ps2_init()
 * call:
 *
 *   extern void ps2mouse_init(void);
 *   ps2mouse_init();
 *
 * NOTE: ps2.c already registers IRQ12 for a basic mouse handler.  If this
 * driver is added alongside an unmodified ps2.c the two IRQ12 registrations
 * will overwrite each other (irq_register_handler() overwrites the slot).
 * The safest approach is to ensure ps2mouse_init() is called AFTER ps2_init()
 * so this driver's richer handler wins.  Alternatively, remove the
 * ps2_mouse_init() call from ps2.c when this driver is in use.
 */

/* Initialise the PS/2 mouse: enable auxiliary port, optional Intellimouse
 * scroll-wheel negotiation, enable data reporting, register IRQ12 handler,
 * and create /dev/input/eventN node via the input subsystem.            */
void ps2mouse_init(void);

/* IRQ12 handler — exposed so the void(void) wrapper can delegate to it.
 * You normally do not call this directly.                                */
void ps2mouse_irq_handler(void);

#endif /* PS2MOUSE_H */
