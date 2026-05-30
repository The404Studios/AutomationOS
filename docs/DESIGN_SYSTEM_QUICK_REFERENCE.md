# Design System Quick Reference

## Spacing (8px Grid)

```
┌─────────────────────────────────────┐
│  SPACING_XS     4px   ████          │
│  SPACING_SM     8px   ████████      │
│  SPACING_MD    16px   ████████████████
│  SPACING_LG    24px   ████████████████████████
│  SPACING_XL    32px   ████████████████████████████████
│  SPACING_XXL   48px   ████████████████████████████████████████████████
└─────────────────────────────────────┘
```

## Corner Radius

```
┌─────────────────────────────────────┐
│  RADIUS_SUBTLE      4px   ╭─╮       │
│  RADIUS_STANDARD    8px   ╭──╮      │
│  RADIUS_PROMINENT  12px   ╭───╮     │
│  RADIUS_DRAMATIC   16px   ╭────╮    │
│  RADIUS_HERO       24px   ╭──────╮  │
└─────────────────────────────────────┘
```

## Typography

```
┌─────────────────────────────────────┐
│  FONT_XS     11px  Caption text     │
│  FONT_SM     12px  Small text       │
│  FONT_BASE   13px  Body text        │
│  FONT_MD     14px  Emphasized       │
│  FONT_LG     16px  Heading          │
│  FONT_XL     20px  Page Title       │
│  FONT_2XL    24px  Hero Text        │
└─────────────────────────────────────┘
```

## Shadows

```
┌─────────────────────────────────────┐
│  SHADOW_SM   ▓    Subtle           │
│  SHADOW      ▓▓   Standard          │
│  SHADOW_MD   ▓▓▓  Medium            │
│  SHADOW_LG   ▓▓▓▓ Large             │
│  SHADOW_XL   ▓▓▓▓▓ Extra Large      │
└─────────────────────────────────────┘
```

## Animations

```
┌─────────────────────────────────────┐
│  ANIM_INSTANT    0ms   No animation │
│  ANIM_FAST     150ms   Quick ▓      │
│  ANIM_NORMAL   250ms   Standard ▓▓  │
│  ANIM_SLOW     400ms   Smooth ▓▓▓   │
│  ANIM_SLOWER   600ms   Dramatic ▓▓▓▓│
└─────────────────────────────────────┘
```

## Light Theme Colors

```
┌─────────────────────────────────────────┐
│  PRIMARY         #007AFF  ████  Blue   │
│  SECONDARY       #5856D6  ████  Purple  │
│  SUCCESS         #34C759  ████  Green   │
│  WARNING         #FF9500  ████  Orange  │
│  ERROR           #FF3B30  ████  Red     │
│                                         │
│  BG_PRIMARY      #FFFFFF  ████  White   │
│  BG_SECONDARY    #F8F8F8  ████  Light   │
│  BG_TERTIARY     #F0F0F0  ████  Medium  │
│                                         │
│  TEXT_PRIMARY    #1C1C1E  ████  Black   │
│  TEXT_SECONDARY  #636366  ████  Gray    │
│  TEXT_TERTIARY   #8E8E93  ████  Light   │
└─────────────────────────────────────────┘
```

## Dark Theme Colors

```
┌─────────────────────────────────────────┐
│  PRIMARY         #0A84FF  ████  Blue    │
│  SECONDARY       #5E5CE6  ████  Purple  │
│  SUCCESS         #32D74B  ████  Green   │
│  WARNING         #FF9F0A  ████  Orange  │
│  ERROR           #FF453A  ████  Red     │
│                                         │
│  BG_PRIMARY      #1C1C1E  ████  Dark    │
│  BG_SECONDARY    #2C2C2E  ████  Medium  │
│  BG_TERTIARY     #3A3A3C  ████  Light   │
│                                         │
│  TEXT_PRIMARY    #FFFFFF  ████  White   │
│  TEXT_SECONDARY  #AEAEB2  ████  Gray    │
│  TEXT_TERTIARY   #8E8E93  ████  Medium  │
└─────────────────────────────────────────┘
```

## Component Sizes

```
┌─────────────────────────────────────┐
│  PANEL_HEIGHT           32px        │
│  DOCK_ICON_SIZE_SM      48px        │
│  DOCK_ICON_SIZE_MD      64px        │
│  DOCK_ICON_SIZE_LG      96px        │
│  BUTTON_HEIGHT_SM       28px        │
│  BUTTON_HEIGHT_MD       32px        │
│  BUTTON_HEIGHT_LG       40px        │
│  INPUT_HEIGHT           32px        │
│  NOTIF_WIDTH           360px        │
│  TOUCH_TARGET_MIN       44px        │
└─────────────────────────────────────┘
```

## Button States

```
┌─────────────────────────────────────┐
│  NORMAL    Scale: 1.0   Opacity: 1.0│
│  HOVER     Scale: 1.05  Color change│
│  ACTIVE    Scale: 0.95  Darker      │
│  DISABLED  Scale: 1.0   Opacity: 0.4│
│  FOCUS     Scale: 1.0   + Ring 2px  │
└─────────────────────────────────────┘
```

## Easing Functions

```
┌─────────────────────────────────────┐
│  EASE_OUT_QUAD     ╱─────            │
│  EASE_OUT_CUBIC    ╱──────           │
│  EASE_OUT_BACK     ╱─╲────  Bounce  │
│  EASE_IN_OUT       ╱────╲            │
│  EASE_SPRING       ╱─╲─╲──  Physics │
└─────────────────────────────────────┘
```

## Z-Index Layers

```
┌─────────────────────────────────────┐
│   0  DESKTOP       Bottom layer     │
│ 100  WINDOWS       App windows      │
│ 200  DOCK          Dock bar         │
│ 300  PANEL         Top panel        │
│ 400  MENUS         Dropdowns        │
│ 500  NOTIFICATIONS Toast messages   │
│ 600  TOOLTIPS      Hover hints      │
│ 700  DIALOGS       Modal dialogs    │
│ 800  POPOVERS      Top layer        │
└─────────────────────────────────────┘
```

## Contrast Ratios (WCAG)

```
┌─────────────────────────────────────┐
│  Normal Text (13px)    4.5:1  AAA   │
│  Large Text (18px+)    3.0:1  AAA   │
│  UI Elements           3.0:1  AA    │
│  Focus Indicators      3.0:1  AA    │
└─────────────────────────────────────┘
```

## Common Patterns

### Button with Hover
```c
button_t btn = {
    .corner_radius = RADIUS_SUBTLE,
    .padding_h = SPACING_MD,
    .height = BUTTON_HEIGHT_MD,
};

// On hover
btn.scale = SCALE_HOVER;  // 1.05
btn.color = theme->colors.primary_hover;

// Animation
animate(ANIM_FAST, EASE_OUT_QUAD);
```

### Panel with Blur
```c
rect_t panel = {
    .height = PANEL_HEIGHT,
    .width = screen_width,
};

apply_blur(&panel, BLUR_STANDARD);  // 20px
draw_rounded_rect(&panel, 0, theme->colors.panel_bg);
```

### Notification Slide-In
```c
notification_t notif = {
    .width = NOTIF_WIDTH,
    .corner_radius = RADIUS_PROMINENT,
};

animate_slide_in(
    from: screen_width,
    to: screen_width - NOTIF_WIDTH - SPACING_MD,
    duration: ANIM_NORMAL,
    easing: EASE_OUT_CUBIC
);
```

### Shadow on Card
```c
rect_t card = {
    .x = SPACING_LG,
    .y = SPACING_LG,
    .width = 300,
    .height = 200,
};

shadow_t shadow = SHADOW_MD;
draw_shadow(&card, shadow, RGB(0, 0, 0));
draw_rounded_rect(&card, RADIUS_STANDARD, theme->colors.bg_secondary);
```

## Quick Checklist

### Every Component Should Have
- [ ] Consistent spacing (8px grid)
- [ ] Proper corner radius
- [ ] Hover state (150ms)
- [ ] Active state (100ms)
- [ ] Focus indicator (2px ring)
- [ ] Disabled state (40% opacity)
- [ ] Smooth animations
- [ ] Accessible contrast (4.5:1)
- [ ] Touch target (44px min)
- [ ] Keyboard support

### Every Animation Should Have
- [ ] 60 FPS performance
- [ ] Appropriate duration (150-600ms)
- [ ] Correct easing function
- [ ] GPU acceleration
- [ ] Reduced motion support

### Every Color Should Have
- [ ] Light and dark variants
- [ ] WCAG contrast check
- [ ] Hover/active variations
- [ ] Semantic meaning
- [ ] Consistent usage

## File Locations

```
userspace/shell/theme/
├── design_system.h      ← All constants here
├── theme_colors.h       ← Color definitions
└── theme_colors.c       ← Color utilities

userspace/lib/ui/
├── button_polished.h    ← Button component
└── render_utils.h       ← Drawing functions

userspace/lib/animation/
└── ui_animations.h      ← Pre-built animations
```

## Usage Example

```c
#include "shell/theme/design_system.h"
#include "shell/theme/theme_colors.h"
#include "lib/ui/render_utils.h"

// Create themed button
rect_t btn = {
    .x = SPACING_MD,           // 16px margin
    .y = SPACING_MD,
    .width = 120,
    .height = BUTTON_HEIGHT_MD // 32px
};

// Get theme colors
theme_colors_t colors;
init_light_theme_colors(&colors);

// Draw with shadow
shadow_t shadow = SHADOW;
draw_shadow(gpu, &btn, shadow, RGB(0, 0, 0));

// Draw button
draw_rounded_rect(gpu, &btn, RADIUS_SUBTLE, colors.primary);

// Draw label
text_style_t style = {
    .font_family = FONT_FAMILY_SYSTEM,
    .font_size = 13,
    .font_weight = 600,
};
draw_text_centered(gpu, "Click Me", &btn, style, RGB(255, 255, 255));

// Animate on hover
if (hovered) {
    animate_scale(btn, 1.0, SCALE_HOVER, ANIM_FAST, EASE_OUT_QUAD);
}
```

## Common Mistakes to Avoid

❌ Using random spacing values
✅ Use spacing constants (4, 8, 16, 24, 32, 48)

❌ Hardcoding colors
✅ Use theme->colors structure

❌ Instant state changes
✅ Animate transitions (150ms minimum)

❌ Low contrast text
✅ Check contrast ratio (4.5:1 minimum)

❌ Tiny touch targets
✅ Minimum 44x44px for buttons

❌ No focus indicators
✅ 2px focus ring on all interactive elements

❌ Linear easing everywhere
✅ Use ease-out for most UI animations

❌ Long animations
✅ Keep under 600ms (250ms is ideal)

## Pro Tips

💡 Use GPU for transforms (scale, rotate, translate)
💡 Batch multiple draw calls together
💡 Cache static textures
💡 Use damage tracking for partial redraws
💡 Test with both light and dark themes
💡 Verify on different DPI scales (1x, 2x, 3x)
💡 Profile animations with FPS counter
💡 Test keyboard navigation early
💡 Use spring physics for natural motion
💡 Add subtle sounds for feedback

---

**Remember**: Every pixel matters. Every interaction should delight.
