#!/usr/bin/env python3
"""
Generate seed corpus for AutomationOS fuzzers

This script creates initial test cases (seeds) for fuzzing.
Good seeds improve fuzzing efficiency by providing interesting starting points.
"""

import os
import struct
import random

def create_directory(path):
    """Create directory if it doesn't exist"""
    os.makedirs(path, exist_ok=True)
    print(f"[CORPUS] Created directory: {path}")

def write_seed(filepath, data):
    """Write binary seed file"""
    with open(filepath, 'wb') as f:
        f.write(data)
    print(f"[CORPUS] Generated seed: {filepath} ({len(data)} bytes)")

# ============================================================================
# Syscall Seeds
# ============================================================================

def generate_syscall_seeds():
    """Generate seeds for syscall fuzzer"""
    corpus_dir = "corpus/syscall_seeds"
    create_directory(corpus_dir)

    # Syscall test case format: syscall_num (4 bytes) + args[6] (6 * 8 bytes)
    # Total: 52 bytes

    # Seed 1: SYS_GETPID (simple, no args)
    data = struct.pack('<I6Q', 8, 0, 0, 0, 0, 0, 0)  # SYS_GETPID = 8
    write_seed(f"{corpus_dir}/seed_getpid", data)

    # Seed 2: SYS_READ with small buffer
    data = struct.pack('<I6Q', 2, 0, 0x1000, 64, 0, 0, 0)  # SYS_READ = 2
    write_seed(f"{corpus_dir}/seed_read_small", data)

    # Seed 3: SYS_READ with large buffer
    data = struct.pack('<I6Q', 2, 0, 0x2000, 1024*1024, 0, 0, 0)
    write_seed(f"{corpus_dir}/seed_read_large", data)

    # Seed 4: SYS_WRITE with small buffer
    data = struct.pack('<I6Q', 3, 1, 0x3000, 128, 0, 0, 0)  # SYS_WRITE = 3
    write_seed(f"{corpus_dir}/seed_write_small", data)

    # Seed 5: Invalid syscall number
    data = struct.pack('<I6Q', 999, 0, 0, 0, 0, 0, 0)
    write_seed(f"{corpus_dir}/seed_invalid_syscall", data)

    # Seed 6: NULL pointer arguments
    data = struct.pack('<I6Q', 2, 0, 0, 100, 0, 0, 0)
    write_seed(f"{corpus_dir}/seed_null_ptr", data)

    # Seed 7: Edge case - max values
    data = struct.pack('<I6Q', 8, 2**64-1, 2**64-1, 2**64-1, 2**64-1, 2**64-1, 2**64-1)
    write_seed(f"{corpus_dir}/seed_max_values", data)

    # Seed 8: Negative integers (sign extension)
    data = struct.pack('<I6q', 2, -1, -1, -1, 0, 0, 0)  # 'q' = signed
    write_seed(f"{corpus_dir}/seed_negative", data)

    # Seed 9: Random mixed
    for i in range(5):
        syscall_num = random.choice([2, 3, 8])  # READ, WRITE, GETPID
        args = [random.randint(0, 0xFFFF) for _ in range(6)]
        data = struct.pack('<I6Q', syscall_num, *args)
        write_seed(f"{corpus_dir}/seed_random_{i}", data)

    print(f"[CORPUS] Generated {14} syscall seeds")

# ============================================================================
# Heap Seeds
# ============================================================================

def generate_heap_seeds():
    """Generate seeds for heap fuzzer"""
    corpus_dir = "corpus/heap_seeds"
    create_directory(corpus_dir)

    # Heap test case format: op (4) + target_idx (4) + size (8) + value (4)
    # Total: 20 bytes

    # Seed 1: Simple allocation
    data = struct.pack('<IIQI', 0, 0, 64, 0)  # OP_ALLOC, idx=0, size=64
    write_seed(f"{corpus_dir}/seed_alloc_small", data)

    # Seed 2: Large allocation
    data = struct.pack('<IIQI', 0, 0, 1024*1024, 0)  # 1MB
    write_seed(f"{corpus_dir}/seed_alloc_large", data)

    # Seed 3: Allocation then free
    data = struct.pack('<IIQI', 0, 0, 256, 0)  # Alloc
    data += struct.pack('<IIQI', 1, 0, 0, 0)   # Free
    write_seed(f"{corpus_dir}/seed_alloc_free", data)

    # Seed 4: Multiple allocations
    for i in range(10):
        data = struct.pack('<IIQI', 0, i, 128 * (i+1), 0)
    write_seed(f"{corpus_dir}/seed_multi_alloc", data)

    # Seed 5: Realloc test
    data = struct.pack('<IIQI', 0, 0, 64, 0)    # Alloc 64 bytes
    data += struct.pack('<IIQI', 2, 0, 128, 0)  # Realloc to 128
    data += struct.pack('<IIQI', 2, 0, 256, 0)  # Realloc to 256
    write_seed(f"{corpus_dir}/seed_realloc", data)

    # Seed 6: Calloc test
    data = struct.pack('<IIQI', 3, 0, 10, 64)  # OP_CALLOC, 10 * 64 bytes
    write_seed(f"{corpus_dir}/seed_calloc", data)

    # Seed 7: Read/write after alloc
    data = struct.pack('<IIQI', 0, 0, 512, 0)  # Alloc
    data += struct.pack('<IIQI', 6, 0, 0, 0)   # Write (OP_WRITE = 6)
    data += struct.pack('<IIQI', 5, 0, 0, 0)   # Read (OP_READ = 5)
    write_seed(f"{corpus_dir}/seed_read_write", data)

    # Seed 8: Fragmentation pattern
    operations = []
    for i in range(20):
        if i % 2 == 0:
            operations.append(struct.pack('<IIQI', 0, i//2, 64, 0))  # Alloc
        else:
            operations.append(struct.pack('<IIQI', 1, i//2, 0, 0))   # Free
    data = b''.join(operations)
    write_seed(f"{corpus_dir}/seed_fragmentation", data)

    # Seed 9: Edge cases
    data = struct.pack('<IIQI', 0, 0, 1, 0)           # Min size
    data += struct.pack('<IIQI', 0, 1, 16*1024*1024, 0)  # Max size
    data += struct.pack('<IIQI', 0, 2, 0, 0)          # Zero size
    write_seed(f"{corpus_dir}/seed_edge_cases", data)

    # Seed 10: Random operations
    for i in range(5):
        operations = []
        for _ in range(50):
            op = random.randint(0, 8)  # OP_MAX = 9
            target = random.randint(0, 99)
            size = random.randint(1, 4096)
            value = random.randint(0, 255)
            operations.append(struct.pack('<IIQI', op, target, size, value))
        data = b''.join(operations)
        write_seed(f"{corpus_dir}/seed_random_{i}", data)

    print(f"[CORPUS] Generated {15} heap seeds")

# ============================================================================
# Driver Seeds
# ============================================================================

def generate_driver_seeds():
    """Generate seeds for driver fuzzer"""
    corpus_dir = "corpus/driver_seeds"
    create_directory(corpus_dir)

    # PS/2 Keyboard Seeds
    # Seed 1: Normal key press
    data = bytes([0x1E])  # 'A' key scancode
    write_seed(f"{corpus_dir}/seed_ps2_keypress", data)

    # Seed 2: Key release
    data = bytes([0x1E | 0x80])  # 'A' key release
    write_seed(f"{corpus_dir}/seed_ps2_keyrelease", data)

    # Seed 3: Shift + key
    data = bytes([0x2A, 0x1E, 0x1E | 0x80, 0x2A | 0x80])  # Shift+A
    write_seed(f"{corpus_dir}/seed_ps2_shift", data)

    # Seed 4: Control character
    data = bytes([0x1D, 0x2E, 0x2E | 0x80, 0x1D | 0x80])  # Ctrl+C
    write_seed(f"{corpus_dir}/seed_ps2_ctrl", data)

    # Seed 5: Rapid keypresses (buffer overflow test)
    data = bytes([random.randint(0, 0x7F) for _ in range(300)])
    write_seed(f"{corpus_dir}/seed_ps2_overflow", data)

    # Serial Port Seeds
    # Seed 6: ASCII text
    data = b"Hello, AutomationOS!\n"
    write_seed(f"{corpus_dir}/seed_serial_text", data)

    # Seed 7: Control characters
    data = bytes([0x00, 0x03, 0x04, 0x08, 0x09, 0x0A, 0x0D, 0x1B, 0x7F])
    write_seed(f"{corpus_dir}/seed_serial_control", data)

    # Seed 8: Binary data
    data = bytes(random.randint(0, 255) for _ in range(256))
    write_seed(f"{corpus_dir}/seed_serial_binary", data)

    # Seed 9: Long line (FIFO overflow)
    data = b"X" * 1000 + b"\n"
    write_seed(f"{corpus_dir}/seed_serial_long", data)

    # Framebuffer Seeds
    # Seed 10: Pixel data (x, y, color)
    data = struct.pack('<III', 100, 100, 0xFF0000)  # Red pixel at (100, 100)
    write_seed(f"{corpus_dir}/seed_fb_pixel", data)

    # Seed 11: Out of bounds
    data = struct.pack('<III', 9999, 9999, 0x00FF00)
    write_seed(f"{corpus_dir}/seed_fb_oob", data)

    # Seed 12: Random pixels
    pixels = []
    for _ in range(100):
        x = random.randint(0, 1920)
        y = random.randint(0, 1080)
        color = random.randint(0, 0xFFFFFF)
        pixels.append(struct.pack('<III', x, y, color))
    data = b''.join(pixels)
    write_seed(f"{corpus_dir}/seed_fb_random", data)

    print(f"[CORPUS] Generated {12} driver seeds")

# ============================================================================
# Main
# ============================================================================

def main():
    print("[CORPUS] Generating seed corpus for AutomationOS fuzzers...")
    print("")

    generate_syscall_seeds()
    print("")
    generate_heap_seeds()
    print("")
    generate_driver_seeds()
    print("")

    print("[CORPUS] Corpus generation complete!")
    print("[CORPUS] Total seeds generated: 41")

if __name__ == "__main__":
    main()
