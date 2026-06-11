/**
 * Simple compilation test for input system
 * Tests that all headers and APIs are syntactically correct
 */

#include "kernel/include/input.h"
#include "kernel/include/irq.h"
#include "kernel/include/syscall.h"
#include "kernel/include/types.h"

// Test that input event structure is defined
void test_input_event(void) {
    input_event_t event;
    event.timestamp = 0;
    event.type = INPUT_EVENT_KEY;
    event.code = KEY_A;
    event.value = 1;
}

// Test that syscall number is defined
void test_syscall(void) {
    uint64_t syscall_num = SYS_READ_EVENT;
    (void)syscall_num;
}

// Test that IRQ handler types are defined
void test_irq(void) {
    irq_return_t ret = IRQ_HANDLED;
    (void)ret;
}

// Test input device functions
void test_input_device(void) {
    input_event_t event;
    int result = input_get_event(&event);
    (void)result;
}

// External PS/2 functions
extern void ps2_get_mouse_position(int32_t* x, int32_t* y);
extern uint8_t ps2_get_mouse_buttons(void);
extern void ps2_set_mouse_position(int32_t x, int32_t y);

void test_ps2_api(void) {
    int32_t x, y;
    ps2_get_mouse_position(&x, &y);
    uint8_t buttons = ps2_get_mouse_buttons();
    ps2_set_mouse_position(100, 100);
    (void)buttons;
}

int main(void) {
    test_input_event();
    test_syscall();
    test_irq();
    test_input_device();
    test_ps2_api();
    return 0;
}
