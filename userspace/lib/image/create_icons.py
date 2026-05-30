#!/usr/bin/env python3
"""
Generate default icon set for AutomationOS desktop
Creates simple PNG icons for terminal, files, settings, etc.
"""

import os
from PIL import Image, ImageDraw, ImageFont

# Icon configurations
ICONS = {
    'terminal': {
        'color': (0, 0, 0),
        'bg': (255, 255, 255),
        'symbol': '>_'
    },
    'folder': {
        'color': (52, 152, 219),  # Blue
        'bg': (255, 255, 255),
        'symbol': None  # Will draw folder shape
    },
    'document': {
        'color': (236, 240, 241),  # Gray
        'bg': (255, 255, 255),
        'symbol': None  # Will draw document shape
    },
    'gear': {
        'color': (149, 165, 166),  # Dark gray
        'bg': (255, 255, 255),
        'symbol': None  # Will draw gear shape
    }
}

SIZES = [16, 24, 32, 48, 64, 96, 128, 256]

def create_terminal_icon(size, color, bg):
    """Create terminal icon with >_ symbol"""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Background rounded rect
    margin = size // 8
    draw.rounded_rectangle(
        [(margin, margin), (size - margin, size - margin)],
        radius=size // 10,
        fill=color,
        outline=None
    )

    # Text
    font_size = size // 3
    try:
        # Try to use a monospace font
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", font_size)
    except:
        try:
            font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", font_size)
        except:
            font = ImageFont.load_default()

    text = ">_"
    bbox = draw.textbbox((0, 0), text, font=font)
    text_width = bbox[2] - bbox[0]
    text_height = bbox[3] - bbox[1]

    x = (size - text_width) // 2
    y = (size - text_height) // 2

    draw.text((x, y), text, fill=bg, font=font)

    return img

def create_folder_icon(size, color, bg):
    """Create folder icon"""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    margin = size // 8
    w = size - 2 * margin
    h = int(w * 0.7)

    # Folder tab
    tab_w = w // 3
    tab_h = h // 4

    # Draw folder
    folder_y = margin + tab_h

    # Tab
    draw.rectangle(
        [(margin, margin + tab_h // 2), (margin + tab_w, folder_y + 2)],
        fill=color
    )

    # Main body
    draw.rounded_rectangle(
        [(margin, folder_y), (margin + w, folder_y + h)],
        radius=size // 20,
        fill=color
    )

    # Lighter shade for depth
    lighter = tuple(min(255, c + 40) for c in color)
    draw.rectangle(
        [(margin + 4, folder_y + 4), (margin + w - 4, folder_y + h // 3)],
        fill=lighter
    )

    return img

def create_document_icon(size, color, bg):
    """Create document/file icon"""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    margin = size // 6
    w = size - 2 * margin
    h = int(w * 1.3)

    fold = w // 4

    # Document body
    draw.rectangle(
        [(margin, margin + fold), (margin + w - fold, margin + h)],
        fill=color,
        outline=(150, 150, 150),
        width=2
    )

    # Folded corner
    points = [
        (margin + w - fold, margin + fold),
        (margin + w - fold, margin),
        (margin + w, margin + fold)
    ]
    draw.polygon(points, fill=(200, 200, 200), outline=(150, 150, 150))

    # Lines (for text)
    if size >= 32:
        line_margin = margin + w // 8
        line_y = margin + fold + size // 8
        line_spacing = size // 10

        for i in range(3):
            y = line_y + i * line_spacing
            if y < margin + h - size // 8:
                draw.line(
                    [(line_margin, y), (margin + w - fold - line_margin, y)],
                    fill=(100, 100, 100),
                    width=max(1, size // 64)
                )

    return img

def create_gear_icon(size, color, bg):
    """Create settings gear icon"""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    center_x = size // 2
    center_y = size // 2
    outer_r = size // 2 - size // 8
    inner_r = outer_r // 2

    # Draw gear teeth (simplified as octagon)
    import math
    teeth = 8
    points = []

    for i in range(teeth * 2):
        angle = (i * math.pi / teeth) - math.pi / 2
        if i % 2 == 0:
            r = outer_r
        else:
            r = outer_r * 0.8

        x = center_x + r * math.cos(angle)
        y = center_y + r * math.sin(angle)
        points.append((x, y))

    draw.polygon(points, fill=color, outline=None)

    # Inner circle
    draw.ellipse(
        [(center_x - inner_r, center_y - inner_r),
         (center_x + inner_r, center_y + inner_r)],
        fill=bg,
        outline=color,
        width=max(2, size // 32)
    )

    # Center hole
    hole_r = inner_r // 2
    draw.ellipse(
        [(center_x - hole_r, center_y - hole_r),
         (center_x + hole_r, center_y + hole_r)],
        fill=(0, 0, 0, 0)
    )

    return img

def create_icon_set(icon_name, config):
    """Create icon set at all sizes"""
    print(f"Creating {icon_name} icon set...")

    base_dir = "../../../assets/icons"
    icon_dir = os.path.join(base_dir, icon_name)
    os.makedirs(icon_dir, exist_ok=True)

    for size in SIZES:
        if icon_name == 'terminal':
            img = create_terminal_icon(size, config['color'], config['bg'])
        elif icon_name == 'folder':
            img = create_folder_icon(size, config['color'], config['bg'])
        elif icon_name == 'document':
            img = create_document_icon(size, config['color'], config['bg'])
        elif icon_name == 'gear':
            img = create_gear_icon(size, config['color'], config['bg'])
        else:
            continue

        output_path = os.path.join(icon_dir, f"{size}x{size}.png")
        img.save(output_path, 'PNG')
        print(f"  Created {output_path}")

def create_default_wallpaper():
    """Create default wallpaper (blue gradient)"""
    print("Creating default wallpaper...")

    width = 1920
    height = 1080

    img = Image.new('RGB', (width, height))
    draw = ImageDraw.Draw(img)

    # Gradient from top to bottom
    for y in range(height):
        # Blue gradient
        r = int(30 + (y / height) * 40)
        g = int(120 + (y / height) * 60)
        b = int(200 + (y / height) * 55)

        draw.line([(0, y), (width, y)], fill=(r, g, b))

    # Add some subtle texture/pattern
    import random
    random.seed(42)
    for _ in range(width * height // 5000):
        x = random.randint(0, width - 1)
        y = random.randint(0, height - 1)

        pixel = img.getpixel((x, y))
        brightness = random.randint(-10, 10)
        new_pixel = tuple(max(0, min(255, c + brightness)) for c in pixel)
        img.putpixel((x, y), new_pixel)

    wallpaper_dir = "../../../assets/wallpapers"
    os.makedirs(wallpaper_dir, exist_ok=True)

    output_path = os.path.join(wallpaper_dir, "default.png")
    img.save(output_path, 'PNG')
    print(f"Created {output_path}")

def main():
    print("AutomationOS Icon Generator")
    print("=" * 40)

    # Create all icon sets
    for icon_name, config in ICONS.items():
        create_icon_set(icon_name, config)

    # Create default wallpaper
    create_default_wallpaper()

    print("\nDone! Icons created in assets/ directory")
    print("\nTo install:")
    print("  sudo mkdir -p /usr/share/icons")
    print("  sudo mkdir -p /usr/share/wallpapers")
    print("  sudo cp -r assets/icons/* /usr/share/icons/")
    print("  sudo cp -r assets/wallpapers/* /usr/share/wallpapers/")

if __name__ == '__main__':
    main()
