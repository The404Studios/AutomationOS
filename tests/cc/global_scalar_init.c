/* cc regression: a global scalar initializer must be emitted (B3 / CC-GLOBALINIT-0).
 * EXPECT: 5
 * Pre-fix symptom: cc_emit_data_section emits `g: dq 0`, so this returns 0 instead of 5. */
int g = 5;

int main(void) {
    return g;
}
