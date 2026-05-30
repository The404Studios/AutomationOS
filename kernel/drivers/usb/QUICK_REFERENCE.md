# USB HID Driver - Quick Reference Card

## Initialization

```c
// In kernel initialization
input_init();
usb_init();

// For testing without hardware
usb_hid_test_init();
```

## Reading Input Events

```c
input_event_t event;
while (input_get_event(&event)) {
    switch (event.type) {
        case INPUT_EVENT_KEY:
            printf("Key: code=%u state=%d\n", event.code, event.value);
            break;
        case INPUT_EVENT_REL:
            printf("Relative: axis=%u delta=%d\n", event.code, event.value);
            break;
        case INPUT_EVENT_ABS:
            printf("Absolute: axis=%u value=%d\n", event.code, event.value);
            break;
    }
}
```

## Key Codes (Most Common)

```c
KEY_A - KEY_Z          // Letters
KEY_0 - KEY_9          // Numbers
KEY_F1 - KEY_F12       // Function keys
KEY_ENTER              // Enter
KEY_BACKSPACE          // Backspace
KEY_SPACE              // Space
KEY_TAB                // Tab
KEY_ESC                // Escape
KEY_LEFTSHIFT          // Left Shift
KEY_RIGHTSHIFT         // Right Shift
KEY_LEFTCTRL           // Left Control
KEY_LEFTALT            // Left Alt
```

## Mouse Axes

```c
REL_X                  // Horizontal movement
REL_Y                  // Vertical movement
REL_WHEEL              // Scroll wheel
BTN_LEFT               // Left button
BTN_RIGHT              // Right button
BTN_MIDDLE             // Middle button
```

## Gamepad Axes

```c
ABS_X, ABS_Y           // Left stick
ABS_RX, ABS_RY         // Right stick
ABS_Z, ABS_RZ          // Triggers
ABS_HAT0X, ABS_HAT0Y   // D-pad
BTN_A, BTN_B           // Face buttons
BTN_X, BTN_Y           // Face buttons
```

## Creating Custom Input Device

```c
// Allocate device
input_device_t* dev = input_allocate_device("My Device");

// Set capabilities
dev->supports_key = true;
dev->supports_rel = true;

// Register
input_register_device(dev);

// Report events
input_report_key(dev, KEY_A, KEY_STATE_PRESSED);
input_report_rel(dev, REL_X, 10);
input_sync(dev);

// Cleanup
input_unregister_device(dev);
input_free_device(dev);
```

## Debug Commands

```c
usb_list_devices();     // List USB devices
usb_list_drivers();     // List USB drivers
input_list_devices();   // List input devices
input_debug_event(&ev); // Print event details
```

## Boot Protocol Reports

### Keyboard (8 bytes)
```
[0] = Modifiers (Ctrl, Shift, Alt)
[1] = Reserved
[2-7] = Up to 6 key codes
```

### Mouse (3-4 bytes)
```
[0] = Buttons (bit 0=left, 1=right, 2=middle)
[1] = X movement (signed)
[2] = Y movement (signed)
[3] = Wheel (optional, signed)
```

## Testing

```c
// Run full test suite
test_usb_hid_suite();

// Individual tests available in test_usb_hid.c
```

## Common Issues

| Issue | Solution |
|-------|----------|
| No devices | Use `usb_hid_test_init()` for simulation |
| No events | Check interrupt transfer started |
| Wrong keys | Verify scancode translation table |
| Queue overflow | Poll events more frequently |

## QEMU Testing

```bash
# Keyboard
qemu-system-x86_64 -kernel kernel.bin -usb -device usb-kbd

# Mouse  
qemu-system-x86_64 -kernel kernel.bin -usb -device usb-mouse

# Both
qemu-system-x86_64 -kernel kernel.bin -usb \
    -device usb-kbd -device usb-mouse
```

## File Locations

```
kernel/drivers/usb/hid.c           - HID driver (800 LOC)
kernel/drivers/usb/usb_core.c      - USB core (300 LOC)
kernel/drivers/input/input.c       - Input system (400 LOC)
kernel/include/usb.h               - USB definitions
kernel/include/input.h             - Input definitions
kernel/testing/test_usb_hid.c      - Test suite
```

## Further Reading

- `kernel/drivers/usb/README.md` - USB driver documentation
- `docs/USB_HID_DRIVER.md` - Complete documentation
- `docs/DRIVER_EXPANSION_PLAN.md` - Phase 3 roadmap

---

**Quick Tip**: Start with `usb_hid_test_init()` for testing without real hardware!
