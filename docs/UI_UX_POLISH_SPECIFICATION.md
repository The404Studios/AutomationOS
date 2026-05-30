# AutomationOS UI/UX Polish Specification

## Mission
Make every pixel perfect and every interaction delightful in AutomationOS.

## Design System

### 1. Spacing System (8px Grid)
```
4px   - XS (icon padding, tight spacing)
8px   - SM (small gaps, compact elements)
16px  - MD (standard padding, comfortable spacing)
24px  - LG (section spacing, generous padding)
32px  - XL (major sections, prominent spacing)
48px  - XXL (hero sections, dramatic spacing)
```

### 2. Corner Radius System
```
4px   - Subtle (buttons, inputs)
8px   - Standard (cards, panels)
12px  - Prominent (dialogs, large cards)
16px  - Dramatic (dock, floating panels)
24px  - Hero (splash screens, special elements)
```

### 3. Shadow System
```
shadow-sm:  0 1px 2px rgba(0,0,0,0.05)
shadow:     0 2px 4px rgba(0,0,0,0.08)
shadow-md:  0 4px 8px rgba(0,0,0,0.12)
shadow-lg:  0 8px 16px rgba(0,0,0,0.16)
shadow-xl:  0 16px 32px rgba(0,0,0,0.20)
```

### 4. Typography System
```
font-xs:      11px / line-height: 1.4  - Captions, labels
font-sm:      12px / line-height: 1.4  - Secondary text
font-base:    13px / line-height: 1.5  - Body text
font-md:      14px / line-height: 1.5  - Emphasized body
font-lg:      16px / line-height: 1.5  - Headings
font-xl:      20px / line-height: 1.4  - Page titles
font-2xl:     24px / line-height: 1.3  - Hero text
```

### 5. Color System Enhancement

#### Light Theme
```c
// Accent Colors
primary:        #007AFF  (Blue - bright, vibrant)
primary-hover:  #0066DD  (Darker on hover)
primary-light:  #E6F2FF  (Light background)

secondary:      #5856D6  (Purple - creative)
success:        #34C759  (Green - positive)
warning:        #FF9500  (Orange - caution)
error:          #FF3B30  (Red - danger)

// Backgrounds (layered depth)
bg-primary:     #FFFFFF  (Base layer)
bg-secondary:   #F8F8F8  (Elevated layer)
bg-tertiary:    #F0F0F0  (Hover states)

// Text (proper contrast)
text-primary:   #1C1C1E  (Main text - 4.5:1 contrast)
text-secondary: #636366  (Secondary text)
text-tertiary:  #8E8E93  (Tertiary text)

// Borders & Dividers
border:         #D1D1D6
border-light:   #E5E5EA
separator:      #C6C6C8

// Overlays
overlay-light:  rgba(0,0,0,0.05)
overlay:        rgba(0,0,0,0.3)
overlay-heavy:  rgba(0,0,0,0.6)
```

#### Dark Theme
```c
// Accent Colors (same as light)
primary:        #0A84FF  (Brighter blue for dark bg)
primary-hover:  #409CFF
primary-light:  #1C3A52

secondary:      #5E5CE6
success:        #32D74B
warning:        #FF9F0A
error:          #FF453A

// Backgrounds
bg-primary:     #1C1C1E  (Base layer)
bg-secondary:   #2C2C2E  (Elevated layer)
bg-tertiary:    #3A3A3C  (Hover states)

// Text
text-primary:   #FFFFFF
text-secondary: #AEAEB2
text-tertiary:  #8E8E93

// Borders & Dividers
border:         #38383A
border-light:   #2C2C2E
separator:      #48484A

// Overlays
overlay-light:  rgba(255,255,255,0.05)
overlay:        rgba(0,0,0,0.5)
overlay-heavy:  rgba(0,0,0,0.8)
```

### 6. Animation System

#### Timing (60 FPS target)
```
instant:   0ms      - No animation
fast:      150ms    - Quick feedback (button clicks)
normal:    250ms    - Standard transitions (menus, fades)
slow:      400ms    - Smooth emphasis (dialogs, slides)
slower:    600ms    - Dramatic effects (page transitions)
```

#### Easing Functions
```
ease-out-quad:   Most UI transitions (buttons, menus)
ease-out-cubic:  Smooth deceleration (slides, fades)
ease-out-back:   Playful overshoot (buttons release)
ease-in-out:     Symmetrical motion (repositioning)
spring:          Natural physics (iOS-like bounces)
```

## Component Polish Checklist

### Panel (Top Bar)
- [ ] Translucent background with blur effect (20px)
- [ ] 32px height with proper vertical centering
- [ ] 8px padding on all interactive elements
- [ ] Smooth hover states (150ms fade)
- [ ] Active states with scale (0.95x)
- [ ] System tray icons: 16x16 with 8px spacing
- [ ] Clock updates every second with smooth fade
- [ ] Drop shadows under hoverable items

### Dock
- [ ] Floating design with rounded corners (16px)
- [ ] Background blur with 90% opacity
- [ ] Magnification effect: 1.0x → 1.5x scale
- [ ] Smooth scale interpolation (250ms ease-out)
- [ ] Running indicator: 4px dot below icon
- [ ] Notification badges: Red circle (10px) top-right
- [ ] Icon shadows: 0 4px 8px rgba(0,0,0,0.2)
- [ ] Spring physics for icon positioning
- [ ] Hover glow effect on icons

### Buttons
- [ ] 8px corner radius
- [ ] Minimum 32px height (44px for touch)
- [ ] 16px horizontal padding
- [ ] Hover: Scale 1.0 → 1.05 (150ms)
- [ ] Active: Scale 1.05 → 0.95 (100ms)
- [ ] Ripple effect on click
- [ ] Focus ring: 2px primary color
- [ ] Disabled state: 40% opacity

### Windows
- [ ] Window shadows: 0 8px 24px rgba(0,0,0,0.15)
- [ ] Title bar: 32px height, blurred background
- [ ] Traffic lights: 12px circles, 8px spacing
- [ ] Corner radius: 12px
- [ ] Resize handles: 4px invisible border
- [ ] Focus effect: Subtle glow
- [ ] Minimize animation: Scale to dock icon
- [ ] Close animation: Fade + scale down

### Menus & Dropdowns
- [ ] Background blur with semi-transparent bg
- [ ] 8px corner radius
- [ ] 4px padding around menu items
- [ ] Menu items: 32px height minimum
- [ ] Hover: Light background fill (100ms)
- [ ] Active: Darker background (100ms)
- [ ] Cascade animation: 50ms stagger per item
- [ ] Drop shadow: 0 8px 16px rgba(0,0,0,0.2)
- [ ] Separator lines: 1px, 50% opacity

### Notifications
- [ ] Slide in from right (250ms ease-out-cubic)
- [ ] Width: 360px fixed
- [ ] Rounded corners: 12px
- [ ] Shadow: 0 4px 12px rgba(0,0,0,0.15)
- [ ] Icon: 40x40 left side
- [ ] Auto-dismiss: 5 seconds default
- [ ] Swipe to dismiss with spring physics
- [ ] Stack vertically with 8px gap

### Tooltips
- [ ] 500ms delay before showing
- [ ] Fade in: 100ms
- [ ] Background: 90% opaque dark
- [ ] Corner radius: 4px
- [ ] Padding: 4px 8px
- [ ] Font size: 11px
- [ ] Pointer arrow: 6px triangle

### Inputs & Text Fields
- [ ] Height: 32px
- [ ] Corner radius: 6px
- [ ] Border: 1px solid border color
- [ ] Focus: 2px primary border + glow
- [ ] Padding: 8px 12px
- [ ] Placeholder: 60% opacity
- [ ] Error state: Red border + shake animation
- [ ] Success state: Green border
- [ ] Smooth transitions: 150ms

### Sliders
- [ ] Track: 4px height, rounded ends
- [ ] Thumb: 20px circle with shadow
- [ ] Active thumb: Scale to 24px
- [ ] Progress fill: Primary color
- [ ] Smooth value changes: 200ms
- [ ] Hover: Show value tooltip
- [ ] Focus ring on thumb

### Switches/Toggles
- [ ] Width: 48px, Height: 28px
- [ ] Pill shape (14px border radius)
- [ ] Knob: 24px circle with shadow
- [ ] Transition: 200ms ease-out-cubic
- [ ] On: Primary color background
- [ ] Off: Gray background
- [ ] Hover: Slight glow effect

### Progress Bars
- [ ] Height: 4px standard, 8px prominent
- [ ] Rounded ends (2px/4px radius)
- [ ] Background: bg-tertiary
- [ ] Fill: Primary color
- [ ] Indeterminate: Shimmer animation
- [ ] Smooth value changes: 300ms

### Checkboxes & Radio Buttons
- [ ] Size: 20x20
- [ ] Corner radius: 4px (checkbox), 50% (radio)
- [ ] Border: 2px solid
- [ ] Check animation: Draw path (150ms)
- [ ] Hover: Scale 1.1x
- [ ] Focus ring: 2px offset

## Accessibility Standards

### Contrast Ratios
- Normal text: 4.5:1 minimum
- Large text (18px+): 3:1 minimum
- Interactive elements: 3:1 minimum
- Focus indicators: High contrast, 2px minimum

### Touch Targets
- Minimum size: 44x44 pixels
- Spacing: 8px between targets
- Visual feedback on all interactions

### Keyboard Navigation
- Visible focus indicators
- Logical tab order
- Escape to close menus/dialogs
- Arrow keys for lists/menus

### Motion
- Respect prefers-reduced-motion
- Option to disable animations
- Essential information without motion

## Performance Targets

### Frame Rate
- Target: 60 FPS (16.67ms per frame)
- Acceptable: 30 FPS (33.33ms per frame)
- Use GPU acceleration for:
  - Blur effects
  - Shadows
  - Transformations (scale, rotate, translate)
  - Opacity changes

### Animation Budget
- Maximum simultaneous animations: 10
- Composite-only animations preferred
- Avoid layout thrashing
- Use transform instead of position changes

### Rendering Optimization
- Damage tracking for partial redraws
- Texture caching for static content
- Batch draw calls
- Cull offscreen elements

## Sound Design

### UI Sounds (Subtle & Pleasant)
```
button_click:       Soft tap (40ms, 1000Hz)
menu_open:          Gentle whoosh (100ms)
notification:       Gentle chime (200ms, harmonious)
error:              Soft error tone (150ms, descending)
success:            Positive chime (200ms, ascending)
window_minimize:    Swoosh down (200ms)
dock_magnify:       Subtle pop (50ms)
```

### Volume Levels
- Default: 30% system volume
- Never intrusive
- User-configurable
- Respect system mute

## Quality Assurance

### Visual Testing
- [ ] Test on different screen sizes
- [ ] Test with light and dark themes
- [ ] Verify color contrast ratios
- [ ] Check alignment on 1x, 2x, 3x DPI
- [ ] Ensure no visual glitches
- [ ] Verify smooth animations

### Interaction Testing
- [ ] Test all hover states
- [ ] Test all click/tap states
- [ ] Test keyboard navigation
- [ ] Test focus indicators
- [ ] Test disabled states
- [ ] Test loading states

### Performance Testing
- [ ] Measure frame rate during animations
- [ ] Check memory usage
- [ ] Profile GPU usage
- [ ] Test with many windows open
- [ ] Verify no animation jank

## Implementation Priority

### Phase 1: Foundation (Critical)
1. Design system constants (spacing, colors, shadows)
2. Theme system refinement
3. Base animation system
4. Shadow and blur rendering

### Phase 2: Core Components (High)
1. Button polish (hover, active, ripple)
2. Panel visual updates
3. Dock magnification and polish
4. Window shadows and corners

### Phase 3: Interactions (Medium)
1. Menu animations
2. Dialog transitions
3. Notification system
4. Tooltip system

### Phase 4: Advanced (Low)
1. Sound effects
2. Advanced animations
3. Micro-interactions
4. Easter eggs

## Inspiration & References
- macOS Big Sur / Monterey design language
- iOS Human Interface Guidelines
- Material Design 3 (motion principles)
- Fluent Design System (blur effects)
- Principles of meaningful transitions
