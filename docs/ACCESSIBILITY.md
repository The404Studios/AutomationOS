# AutomationOS Accessibility Guide

**WCAG 2.1 Level AA Compliant**

AutomationOS is designed to be accessible to everyone, regardless of ability. This guide covers all accessibility features and how to use them.

## Quick Reference

| Feature | Keyboard Shortcut | Setting Location |
|---------|------------------|------------------|
| High Contrast | `Ctrl+Alt+H` | Settings → Accessibility → Visual |
| Text Size | `Ctrl++` / `Ctrl+-` | Settings → Accessibility → Visual |
| Screen Reader | `Ctrl+Alt+S` | Settings → Accessibility → Visual |
| Keyboard Nav | `Tab` / `Shift+Tab` | Always enabled |
| Sticky Keys | Press `Shift` 5x | Settings → Accessibility → Motor |
| Visual Alerts | N/A | Settings → Accessibility → Auditory |

---

## Table of Contents

1. [Visual Accessibility](#visual-accessibility)
2. [Motor Accessibility](#motor-accessibility)
3. [Auditory Accessibility](#auditory-accessibility)
4. [Cognitive Accessibility](#cognitive-accessibility)
5. [Quick Profiles](#quick-profiles)
6. [WCAG 2.1 Compliance](#wcag-21-compliance)
7. [Testing & Validation](#testing--validation)
8. [Developer Guide](#developer-guide)

---

## Visual Accessibility

### High Contrast Mode

Makes text and UI elements more visible with increased contrast.

**How to enable:**
1. Open Settings → Accessibility
2. Toggle "High Contrast Mode" ON
3. Choose contrast mode:
   - **High Contrast**: Black/white with high contrast
   - **Inverted Colors**: Inverts all colors
   - **Custom**: Define your own colors

**WCAG Requirement:** ✓ 4.5:1 contrast ratio for normal text, 3:1 for large text (18pt+)

### Text Size

Resize all text without breaking layout.

**Supported scales:**
- 100% (Default)
- 125% (Large)
- 150% (Larger)
- 200% (Extra Large)

**How to adjust:**
- Settings → Accessibility → Text Size slider
- Keyboard: `Ctrl++` (increase), `Ctrl+-` (decrease)

**WCAG Requirement:** ✓ Text resizable up to 200% without loss of content or functionality

### Color Blind Modes

Simulates and compensates for different types of color blindness.

**Modes:**
- **Protanopia** (Red-blind): 1% of males
- **Deuteranopia** (Green-blind): 1% of males  
- **Tritanopia** (Blue-blind): <0.01%
- **Achromatopsia** (Total color blindness): Rare

**How to enable:**
1. Settings → Accessibility
2. Color Blind Mode dropdown
3. Select your type

**Note:** UI never relies on color alone to convey information.

### Screen Reader

Text-to-speech for all UI elements.

**Features:**
- Announces all interactive elements
- Describes buttons, menus, windows
- Reads notifications and alerts
- Keyboard-friendly navigation

**How to enable:**
- Settings → Accessibility → Screen Reader toggle
- Keyboard: `Ctrl+Alt+S`

**Supported commands:**
- `Tab`: Next element
- `Shift+Tab`: Previous element
- `Ctrl+Home`: Jump to top
- `Ctrl+End`: Jump to bottom

### Reduce Motion

Minimizes or disables animations for motion sensitivity.

**How to enable:**
- Settings → Accessibility → Reduce Motion toggle

**Effects:**
- Disables window animations
- Removes parallax effects
- Ensures no flashing >3 times per second (seizure safety)

**WCAG Requirement:** ✓ Max 3 flashes per second

### Cursor Enhancements

**Options:**
- **Cursor Size**: Normal, Large, Extra Large
- **Cursor Highlight**: Shows a ring around cursor
- **Blink Rate**: Adjust or disable blinking (0-1000ms)

**How to adjust:**
- Settings → Accessibility → Cursor section

---

## Motor Accessibility

### Full Keyboard Navigation

Navigate the entire OS without a mouse.

**Always enabled** - No configuration needed.

**Navigation:**
- `Tab`: Next focusable element
- `Shift+Tab`: Previous element
- `Enter`: Activate button/link
- `Space`: Toggle checkbox/switch
- `Esc`: Cancel/close
- `Arrow keys`: Navigate lists/menus
- `Ctrl+Tab`: Switch windows
- `Alt+Tab`: Switch applications

**Focus indicators:**
- Always visible (2px blue outline)
- Never hidden or low contrast
- Follows WCAG guidelines

**WCAG Requirement:** ✓ All functionality available via keyboard

### Sticky Keys

Press modifier keys (Ctrl, Alt, Shift) one at a time instead of holding.

**How to enable:**
- Press `Shift` 5 times rapidly, OR
- Settings → Accessibility → Sticky Keys

**How it works:**
1. Press `Ctrl` (modifier is "sticky")
2. Press `C` (copies)
3. No need to hold `Ctrl`

**Options:**
- **Beep on activation**: Audio feedback
- **Lock mode**: Press modifier twice to lock until pressed again

### Slow Keys

Ignore brief key presses (prevents accidental key presses).

**How to enable:**
- Settings → Accessibility → Slow Keys

**Default delay:** 300ms

**How it works:**
- Must hold key for 300ms to register
- Prevents accidental touches
- Configurable: 100ms - 2000ms

### Bounce Keys

Ignores rapid repeated key presses.

**How to enable:**
- Settings → Accessibility → Bounce Keys

**Default delay:** 500ms

**How it works:**
- Ignores same key pressed <500ms apart
- Prevents accidental double-press
- Configurable: 100ms - 2000ms

### Mouse Keys

Control mouse cursor with keyboard numpad.

**How to enable:**
- Settings → Accessibility → Mouse Keys

**Numpad controls:**
- `8`: Move up
- `2`: Move down
- `4`: Move left
- `6`: Move right
- `7`,`9`,`1`,`3`: Diagonal
- `5`: Click
- `+`: Double click
- `0`: Hold button
- `/`: Left button
- `*`: Both buttons
- `-`: Right button

**Speed:** Adjustable 1-10 (default: 5)

### Dwell Click

Click by hovering the cursor (no physical click needed).

**How to enable:**
- Settings → Accessibility → Dwell Click

**Dwell time:** 1200ms default (configurable 500-3000ms)

**How it works:**
1. Move cursor over button
2. Hold still for 1.2 seconds
3. Automatic click

### On-Screen Keyboard

Virtual keyboard controlled by mouse or touch.

**How to enable:**
- Settings → Accessibility → On-Screen Keyboard

**Features:**
- Resizable (50%-100% of screen width)
- Includes modifier keys
- Click or dwell-click keys
- Word prediction (optional)

---

## Auditory Accessibility

### Visual Alerts

Flash the screen or taskbar for system sounds.

**How to enable:**
- Settings → Accessibility → Visual Alerts

**Options:**
- **Flash screen**: Whole screen flashes
- **Flash taskbar**: Only taskbar flashes (less distracting)

**Events:**
- System notifications
- Alerts and warnings
- Incoming messages
- Error sounds

### Closed Captions

Display text for all system audio.

**How to enable:**
- Settings → Accessibility → Captions

**Caption settings:**
- Font: sans-serif (default)
- Size: 16pt (default, adjustable)
- Position: Bottom center
- Background: Semi-transparent black

### Mono Audio

Convert stereo audio to mono (useful for single-ear hearing).

**How to enable:**
- Settings → Accessibility → Mono Audio

**Effect:** Both left and right channels play the same mixed audio.

### Audio Balance

Adjust left/right audio balance.

**How to adjust:**
- Settings → Accessibility → Audio Balance slider
- Range: 100% Left ↔ 100% Right

### Volume Amplification

Boost audio volume beyond normal maximum.

**How to adjust:**
- Settings → Accessibility → Audio Amplification
- Range: 100% (normal) to 300% (3x boost)

**Warning:** Very high amplification may cause distortion.

---

## Cognitive Accessibility

### Simple Mode

Simplified UI with larger buttons and clearer language.

**How to enable:**
- Settings → Accessibility → Simple Mode

**Changes:**
- Larger buttons (min 48×48px)
- Simpler language (less jargon)
- Consistent layout
- Reduced visual clutter
- One task per screen

### Focus Assist

Minimize distractions for better concentration.

**How to enable:**
- Settings → Accessibility → Focus Assist

**Effects:**
- Blocks notifications (except priority)
- Hides animations
- Grays out non-active windows
- Removes badges/indicators

### Extended Timeouts

More time to complete actions before timeout.

**How to enable:**
- Settings → Accessibility → Extended Timeouts

**Multipliers:**
- 2× (default)
- 3×
- 5×
- No timeout

### Reading Assistance

**Features:**
- **Dyslexia Font**: OpenDyslexic font
- **Reading Guide**: Highlights current line
- **Auto-scroll**: Configurable reading speed (WPM)

**How to enable:**
- Settings → Accessibility → Reading section

---

## Quick Profiles

Pre-configured accessibility settings for common needs.

### Low Vision Profile

**Includes:**
- Text size: 200%
- High contrast mode
- Cursor: Extra large
- Screen reader: Enabled
- Reduce motion: Enabled

**To activate:** Settings → Accessibility → Quick Profiles → Low Vision

### Motor Impairment Profile

**Includes:**
- Sticky keys: Enabled
- Slow keys: Enabled (300ms)
- Keyboard navigation: Enabled
- Focus indicators: Always visible
- Dwell click: Enabled

**To activate:** Settings → Accessibility → Quick Profiles → Motor Impairment

### Hearing Impairment Profile

**Includes:**
- Visual alerts: Enabled
- Captions: Enabled
- Mono audio: Enabled
- Audio amplification: 150%

**To activate:** Settings → Accessibility → Quick Profiles → Hearing Impairment

### Cognitive Profile

**Includes:**
- Simple mode: Enabled
- Focus assist: Enabled
- Extended timeouts: 3×
- Reduce motion: Enabled
- Large buttons: Enabled

**To activate:** Settings → Accessibility → Quick Profiles → Cognitive

---

## WCAG 2.1 Compliance

AutomationOS meets **WCAG 2.1 Level AA** standards.

### Summary of Compliance

| Guideline | Requirement | Status |
|-----------|-------------|--------|
| **1.1 Text Alternatives** | Alt text for images | ✓ Compliant |
| **1.3 Adaptable** | Semantic structure | ✓ Compliant |
| **1.4.3 Contrast** | 4.5:1 normal, 3:1 large | ✓ Compliant |
| **1.4.4 Resize Text** | Up to 200% | ✓ Compliant |
| **1.4.5 Images of Text** | Use real text | ✓ Compliant |
| **2.1 Keyboard** | All functionality | ✓ Compliant |
| **2.1.2 No Keyboard Trap** | Focus movable | ✓ Compliant |
| **2.4.3 Focus Order** | Logical order | ✓ Compliant |
| **2.4.7 Focus Visible** | Always visible | ✓ Compliant |
| **2.5.5 Target Size** | Min 44×44px | ✓ Compliant |
| **3.2.3 Consistent Navigation** | Consistent | ✓ Compliant |
| **3.2.4 Consistent Identification** | Consistent | ✓ Compliant |
| **3.3 Input Assistance** | Error prevention | ✓ Compliant |

### Tested By

- Automated: `accessibility_validator` tool
- Manual: Keyboard-only testing
- Screen reader: Compatible with NVDA, JAWS
- Color blind: Coblis simulator

---

## Testing & Validation

### Running Accessibility Tests

AutomationOS includes an automated testing tool:

```bash
# Run all accessibility tests
./tools/accessibility_validator

# Output: accessibility_report.txt
```

### Test Coverage

The validator checks:

1. **Contrast Ratios** (10 test cases)
   - Black/white (21:1)
   - System colors (all pass 4.5:1)
   - Error states (visible contrast)

2. **Touch Targets** (6 test cases)
   - Buttons: 44×44px minimum
   - Icons with padding
   - List items

3. **Keyboard Navigation**
   - Tab order logical
   - All elements reachable
   - Focus visible
   - No keyboard traps

4. **Text Resize**
   - 100%, 125%, 150%, 200%
   - No layout breaks
   - No content loss

5. **Color Blind Filters**
   - Protanopia, Deuteranopia, Tritanopia, Achromatopsia
   - Color not sole differentiator

6. **Screen Reader**
   - All elements labeled
   - Announcements work
   - Semantic HTML

7. **Reduced Motion**
   - Flash rate <3/sec
   - Animations optional
   - No seizure risk

### Manual Testing Checklist

- [ ] Navigate entire OS with keyboard only (no mouse)
- [ ] Use with high contrast mode enabled
- [ ] Use with text size at 200%
- [ ] Test with screen reader (NVDA/JAWS)
- [ ] Test with color blind simulation (Coblis)
- [ ] Verify focus indicators always visible
- [ ] Check for flashing content (must be <3/sec)
- [ ] Verify all buttons ≥44×44px
- [ ] Test sticky keys functionality
- [ ] Test visual alerts (no audio)

---

## Developer Guide

### Integrating Accessibility

Include the accessibility framework in your app:

```c
#include "userspace/lib/accessibility/accessibility.h"

// Initialize
accessibility_context_t *a11y = accessibility_init();

// Check if high contrast enabled
if (a11y->visual.contrast_mode == CONTRAST_MODE_HIGH) {
    // Use high contrast colors
}

// Check text scale
float scale = a11y->visual.text_scale;
uint32_t font_size = BASE_FONT_SIZE * scale;

// Cleanup
accessibility_cleanup(a11y);
```

### Ensuring Contrast Compliance

```c
// Check contrast ratio
uint32_t fg_color = 0x007AFF;  // Blue
uint32_t bg_color = 0xFFFFFF;  // White
bool large_text = false;

float ratio = accessibility_calculate_contrast_ratio(fg_color, bg_color);
bool compliant = accessibility_meets_wcag_contrast(ratio, large_text);

if (!compliant) {
    // Use high contrast alternative
    fg_color = accessibility_apply_high_contrast(fg_color, bg_color);
}
```

### Making Elements Focusable

```c
focusable_element_t button = {
    .element_id = 1,
    .bounds = {100, 100, 120, 44},
    .tab_index = 0,
    .focusable = true,
    .role = "button",
    .label = "Save"
};

accessibility_register_focusable(a11y, &button);

// Handle focus
uint32_t focused = accessibility_get_focused_element(a11y);
if (focused == button.element_id) {
    // Draw focus indicator
}
```

### Screen Reader Support

```c
// Announce text
accessibility_announce(a11y, "Settings saved successfully");

// Describe element
accessibility_describe_element(a11y, "button", "Save", "clickable");

// Register callback
void my_screen_reader(const char *text, void *data) {
    printf("Screen Reader: %s\n", text);
}
accessibility_register_screen_reader(my_screen_reader, NULL);
```

### Validating Touch Targets

```c
uint32_t button_width = 44;
uint32_t button_height = 44;

if (!accessibility_validate_touch_target(button_width, button_height)) {
    fprintf(stderr, "Button too small! Must be ≥44×44px\n");
}
```

### Color Blind Simulation

```c
uint32_t original_color = 0xFF0000;  // Red

// Simulate for protanopia (red-blind)
uint32_t simulated = accessibility_apply_color_blind_filter(
    original_color,
    COLOR_BLIND_PROTANOPIA
);

// Use simulated color in preview mode
```

---

## Keyboard Shortcuts Reference

### Global Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Alt+H` | Toggle high contrast |
| `Ctrl+Alt+S` | Toggle screen reader |
| `Ctrl++` | Increase text size |
| `Ctrl+-` | Decrease text size |
| `Ctrl+Alt+K` | Toggle on-screen keyboard |
| `Shift` (5×) | Enable sticky keys |

### Navigation Shortcuts

| Shortcut | Action |
|----------|--------|
| `Tab` | Next element |
| `Shift+Tab` | Previous element |
| `Enter` | Activate |
| `Space` | Toggle |
| `Esc` | Cancel |
| `Ctrl+Tab` | Next window |
| `Ctrl+Shift+Tab` | Previous window |
| `Alt+Tab` | Switch app |
| `Alt+F4` | Close window |

### Window Management

| Shortcut | Action |
|----------|--------|
| `Win+Left` | Snap left |
| `Win+Right` | Snap right |
| `Win+Up` | Maximize |
| `Win+Down` | Restore/minimize |
| `Win+D` | Show desktop |
| `Win+M` | Minimize all |

---

## Additional Resources

- **WCAG 2.1 Guidelines**: https://www.w3.org/WAI/WCAG21/quickref/
- **WebAIM**: https://webaim.org/
- **A11y Project**: https://www.a11yproject.com/
- **NVDA Screen Reader**: https://www.nvaccess.org/
- **Coblis Color Blind Simulator**: https://www.color-blindness.com/coblis-color-blindness-simulator/

---

## Support

For accessibility issues or feature requests:

- GitHub Issues: [AutomationOS Issues](https://github.com/automationos/kernel/issues)
- Email: accessibility@automationos.org
- Accessibility Statement: [Link]

---

**Last Updated:** 2026-05-26  
**WCAG Version:** 2.1 Level AA  
**Compliance Status:** ✓ Compliant
