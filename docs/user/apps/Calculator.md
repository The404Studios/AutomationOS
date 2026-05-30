# Calculator User Guide

**Application:** Calculator  
**Version:** 1.0.0  
**Category:** Utilities  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Overview](#overview)
2. [Interface](#interface)
3. [Basic Mode](#basic-mode)
4. [Scientific Mode](#scientific-mode)
5. [Programmer Mode](#programmer-mode)
6. [Unit Converter](#unit-converter)
7. [Keyboard Shortcuts](#keyboard-shortcuts)
8. [History](#history)
9. [Tips and Tricks](#tips-and-tricks)

---

## Overview

The Calculator application provides comprehensive calculation capabilities for everyday math, scientific calculations, programming operations, and unit conversions.

**Features:**
- Basic arithmetic operations
- Scientific functions (trig, log, exp)
- Programmer mode (binary, hex, oct)
- Unit conversions
- Calculation history
- Keyboard support
- Copy/paste results

**Opening Calculator:**
- Application Menu → Calculator
- Search for "Calculator"
- Terminal: `calculator`

---

## Interface

```
┌────────────────────────────────┐
│  Calculator              [-□✕] │
├────────────────────────────────┤
│  [Basic][Scientific][Programmer]│ ← Mode tabs
├────────────────────────────────┤
│                     1234567890 │ ← Display
├────────────────────────────────┤
│  [CE] [C]  [⌫]  [÷]           │
│  [7]  [8]  [9]  [×]           │ ← Keypad
│  [4]  [5]  [6]  [−]           │
│  [1]  [2]  [3]  [+]           │
│  [±]  [0]  [.]  [=]           │
└────────────────────────────────┘
```

**Components:**

**Display:**
- Shows current number
- Up to 15 digits
- Scientific notation for large/small numbers
- Right-aligned

**Mode Tabs:**
- Basic: Standard calculator
- Scientific: Advanced math functions
- Programmer: Binary/hex/octal
- Converter: Unit conversions

**Keypad:**
- Numbers 0-9
- Operators (+, −, ×, ÷)
- Decimal point
- Clear/Backspace
- Equals

---

## Basic Mode

Basic arithmetic operations for everyday calculations.

### Operations

**Addition (+):**
```
15 + 7 = 22
```

**Subtraction (−):**
```
25 − 8 = 17
```

**Multiplication (×):**
```
6 × 9 = 54
```

**Division (÷):**
```
100 ÷ 5 = 20
```

**Percentage (%):**
```
50 % of 200 = 100
```

### Buttons

**Number Buttons (0-9):**
- Click to enter digits
- Build multi-digit numbers

**Decimal Point (.):**
- Enter decimal numbers
- Example: 3.14159

**Operators (+, −, ×, ÷):**
- Click between numbers
- Chain operations

**Equals (=):**
- Complete calculation
- Show result

**Clear (C):**
- Clear all and start over
- Resets to 0

**Clear Entry (CE):**
- Clear current number
- Keep previous calculation

**Backspace (⌫):**
- Delete last digit
- Correct typos

**Sign Change (±):**
- Toggle positive/negative
- Example: 5 → −5

### Example Calculations

**Simple:**
```
45 + 23 = 68
```

**Chained:**
```
10 + 5 − 3 = 12
```

**Decimals:**
```
3.5 × 2.8 = 9.8
```

**Percentage:**
```
20% of 150:
150 × 20% = 30
```

**Tip Calculation:**
```
Bill: $85
Tip: 18%
85 × 1.18 = $100.30
```

---

## Scientific Mode

Advanced mathematical functions for science and engineering.

### Interface

```
┌──────────────────────────────────┐
│                     3.141592654  │
├──────────────────────────────────┤
│  [sin][cos][tan][sinh][cosh][tanh│
│  [ln] [log][e^x][10^x][x²] [x^y] │
│  [√]  [∛] [x!] [1/x][π]  [e]    │
│  [(]  [)]  [CE] [C]  [⌫]  [÷]   │
│  [7]  [8]  [9]  [×]              │
│  [4]  [5]  [6]  [−]              │
│  [1]  [2]  [3]  [+]              │
│  [±]  [0]  [.]  [=]              │
└──────────────────────────────────┘
```

### Trigonometric Functions

**sin (Sine):**
```
sin(30°) = 0.5
sin(π/6) = 0.5  (radians)
```

**cos (Cosine):**
```
cos(60°) = 0.5
cos(π/3) = 0.5
```

**tan (Tangent):**
```
tan(45°) = 1
tan(π/4) = 1
```

**Inverse Functions:**
- sin⁻¹ (arcsin)
- cos⁻¹ (arccos)
- tan⁻¹ (arctan)

**Angle Mode:**
- Degrees (default)
- Radians
- Gradians

Toggle: View → Angle Mode

### Logarithmic Functions

**Natural Log (ln):**
```
ln(e) = 1
ln(10) = 2.302585
```

**Common Log (log):**
```
log(100) = 2
log(1000) = 3
```

**Exponential:**
```
e^x: e^2 = 7.389056
10^x: 10^3 = 1000
```

### Power Functions

**Square (x²):**
```
5² = 25
```

**Cube (x³):**
```
3³ = 27
```

**Power (x^y):**
```
2^8 = 256
```

**Square Root (√):**
```
√144 = 12
```

**Cube Root (∛):**
```
∛27 = 3
```

**Nth Root:**
```
∜81 = 3  (4th root)
```

### Other Functions

**Factorial (!):**
```
5! = 120
```

**Reciprocal (1/x):**
```
1/5 = 0.2
```

**Constants:**
- π = 3.141592654...
- e = 2.718281828...

**Parentheses ( ):**
- Group operations
- Control order

### Example Calculations

**Pythagorean Theorem:**
```
c = √(a² + b²)
a = 3, b = 4
3 x² + 4 x² = 9 + 16 = 25
√ = 5
```

**Compound Interest:**
```
A = P(1 + r)^n
P = 1000, r = 0.05, n = 10
1000 × 1.05 x^y 10 = 1628.89
```

**Trigonometry:**
```
Height of tower:
tan(30°) = h/100
h = 100 × tan(30°) = 57.735 m
```

---

## Programmer Mode

Binary, hexadecimal, and octal calculations for programmers.

### Interface

```
┌──────────────────────────────────┐
│ Hex │ Dec │ Oct │ Bin            │ ← Number system tabs
├──────────────────────────────────┤
│ HEX  0x1A4                       │ ← Display (current system)
│ DEC  420                         │ ← Decimal equivalent
│ BIN  110100100                   │ ← Binary equivalent
├──────────────────────────────────┤
│ [A] [B] [C] [D] [E] [F]         │ ← Hex buttons
│ [7] [8] [9] [÷] [AND][OR] [XOR] │
│ [4] [5] [6] [×] [NOT][<<] [>>]  │
│ [1] [2] [3] [−] [1's][2's][Mod] │
│ [0] [.]     [+] [=]              │
└──────────────────────────────────┘
```

### Number Systems

**Hexadecimal (Base 16):**
- Digits: 0-9, A-F
- Prefix: 0x
- Example: 0x1A = 26 (decimal)

**Decimal (Base 10):**
- Standard numbers
- No prefix
- Example: 42

**Octal (Base 8):**
- Digits: 0-7
- Prefix: 0o
- Example: 0o52 = 42 (decimal)

**Binary (Base 2):**
- Digits: 0, 1
- Prefix: 0b
- Example: 0b101010 = 42 (decimal)

### Conversion

**Automatic Display:**
All systems shown simultaneously:

```
Input: 255 (decimal)
HEX: 0xFF
DEC: 255
OCT: 0o377
BIN: 0b11111111
```

**Switching Systems:**
- Click system tab
- Changes input/output format
- Automatic conversion

### Bitwise Operations

**AND:**
```
0b1100 AND 0b1010 = 0b1000
(12 AND 10 = 8)
```

**OR:**
```
0b1100 OR 0b1010 = 0b1110
(12 OR 10 = 14)
```

**XOR (Exclusive OR):**
```
0b1100 XOR 0b1010 = 0b0110
(12 XOR 10 = 6)
```

**NOT:**
```
NOT 0b1010 = 0b0101
(NOT 10 = 5, for 4-bit)
```

**Left Shift (<<):**
```
0b0011 << 2 = 0b1100
(3 << 2 = 12)
Multiply by 2^n
```

**Right Shift (>>):**
```
0b1100 >> 2 = 0b0011
(12 >> 2 = 3)
Divide by 2^n
```

### Complement Operations

**1's Complement:**
```
0b1010 → 0b0101
Flip all bits
```

**2's Complement:**
```
0b1010 → 0b0110
1's complement + 1
Used for negative numbers
```

### Word Size

View → Word Size

**Options:**
- 8-bit (byte): 0-255
- 16-bit (word): 0-65535
- 32-bit (dword): 0-4294967295
- 64-bit (qword): 0-18446744073709551615

**Affects:**
- Maximum value
- NOT operation
- Bit display

### Example Calculations

**Permissions (Unix):**
```
rwxr-xr-x = 755
Owner: rwx = 111 = 7
Group: r-x = 101 = 5
Other: r-x = 101 = 5
Result: 0o755
```

**IP Address:**
```
192.168.1.1
192: 0xC0
168: 0xA8
001: 0x01
001: 0x01
Result: 0xC0A80101
```

**Bit Masking:**
```
Value: 0b11011010
Mask:  0b00001111
AND:   0b00001010
Extract lower 4 bits
```

---

## Unit Converter

Convert between different units of measurement.

### Interface

```
┌──────────────────────────────────┐
│  Unit Converter                  │
├──────────────────────────────────┤
│  Category: [Length      ▼]       │
│                                  │
│  From: [Meters         ▼]        │
│  100                             │
│           ⇅                      │
│  To:   [Feet           ▼]        │
│  328.084                         │
└──────────────────────────────────┘
```

### Categories

**Length:**
- Millimeters, Centimeters, Meters, Kilometers
- Inches, Feet, Yards, Miles
- Nautical Miles

**Weight/Mass:**
- Milligrams, Grams, Kilograms, Tonnes
- Ounces, Pounds, Stones

**Volume:**
- Milliliters, Liters
- Fluid Ounces, Cups, Pints, Quarts, Gallons

**Temperature:**
- Celsius, Fahrenheit, Kelvin

**Area:**
- Square meters, Square kilometers
- Square feet, Square yards, Acres

**Speed:**
- Meters/second, Kilometers/hour
- Miles/hour, Knots

**Time:**
- Seconds, Minutes, Hours, Days, Weeks, Years

**Energy:**
- Joules, Kilojoules
- Calories, Kilocalories

**Power:**
- Watts, Kilowatts
- Horsepower

**Pressure:**
- Pascals, Kilopascals
- PSI, Bar, Atmospheres

**Data:**
- Bits, Bytes, Kilobytes, Megabytes, Gigabytes, Terabytes
- Binary (KiB, MiB, GiB, TiB)

### Usage

1. Select category
2. Choose "From" unit
3. Enter value
4. Choose "To" unit
5. Result appears instantly

**Reverse:**
- Click ⇅ button
- Swaps from/to units

### Examples

**Temperature:**
```
32°F = 0°C = 273.15K
```

**Distance:**
```
1 mile = 1.60934 km
100 km = 62.137 miles
```

**Data:**
```
1 GB = 1000 MB (decimal)
1 GiB = 1024 MiB (binary)
```

**Cooking:**
```
1 cup = 236.588 mL
250 mL ≈ 1.057 cups
```

---

## Keyboard Shortcuts

Efficient calculator use with keyboard:

### Numbers and Operators

```
0-9     Enter digits
.       Decimal point
+ - * / Basic operators
Enter   Equals (=)
=       Equals
```

### Functions

```
Backspace    Delete last digit (⌫)
Delete       Clear Entry (CE)
Escape       Clear All (C)
c            Clear All
```

### Advanced

```
s       sin
o       cos
t       tan
l       ln (natural log)
g       log (common log)
q       Square root
@       Square
!       Factorial
p       Pi (π)
e       Euler's number (e)
r       Reciprocal (1/x)
%       Percent
```

### Mode Switching

```
Ctrl+1  Basic mode
Ctrl+2  Scientific mode
Ctrl+3  Programmer mode
Ctrl+4  Converter mode
```

### Copy/Paste

```
Ctrl+C  Copy result
Ctrl+V  Paste number
Ctrl+X  Cut result
```

### History

```
Ctrl+H  Show history
Up      Previous calculation
Down    Next calculation
```

---

## History

View and reuse previous calculations.

**Opening History:**
- View → History (Ctrl+H)
- Or: Click history icon

**Interface:**

```
┌──────────────────────────┐
│  Calculation History     │
├──────────────────────────┤
│  45 + 23 = 68           │
│  100 ÷ 5 = 20           │
│  √144 = 12              │
│  2^8 = 256              │
└──────────────────────────┘
```

**Features:**

**Reuse Calculation:**
- Click entry
- Loads into calculator

**Copy Result:**
- Right-click → Copy Result

**Copy Calculation:**
- Right-click → Copy Full Calculation

**Clear History:**
- Right-click → Clear History
- Or: Edit → Clear History

**Export History:**
- File → Export History
- Save as text file

**Persistent:**
- History saved between sessions
- Maximum 100 entries

---

## Tips and Tricks

**Tip 1: Chain Calculations**
```
No need to press = each time:
5 + 3 + 2 = 10
Keep pressing + after numbers
```

**Tip 2: Quick Percentage**
```
Find 15% of 200:
200 × 15 % = 30
```

**Tip 3: Scientific Notation**
```
Large numbers automatic:
999999999999999 → 1E+15
```

**Tip 4: Parentheses**
```
Complex calculations:
(5 + 3) × (10 − 2) = 64
```

**Tip 5: Memory Functions**
```
Store intermediate results:
M+  Add to memory
M-  Subtract from memory
MR  Recall memory
MC  Clear memory
```

**Tip 6: Constant Operations**
```
Multiply many numbers by same constant:
5 × 2 = 10
6 = 12  (continues × 2)
7 = 14
```

**Tip 7: Quick Conversions**
```
In converter mode:
Edit both fields directly
No need for buttons
```

**Tip 8: Angle Mode**
```
Remember current mode:
Degrees for everyday use
Radians for calculus/physics
```

**Tip 9: Programmer Quick Entry**
```
Type prefix directly:
0xFF → enters hex
0b1010 → enters binary
0o77 → enters octal
```

**Tip 10: Keyboard Efficiency**
```
Use number pad for speed
Keep one hand on mouse
Other on number pad
```

---

**Document Information**

- **Application:** Calculator
- **Version:** 1.0.0
- **Lines:** ~600
- **Last Updated:** 2026-05-26
- **Category:** Application Guide
