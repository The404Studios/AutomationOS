#!/usr/bin/env python3
"""
gen_stub_kernel.py -- Generate a minimal 64-bit ELF stub kernel for ABLoader testing.

The stub:
  - Is a valid ELF64 executable
  - Has one PT_LOAD segment at physical address 0x100000 (1MB)
  - Entry point: 0x100000
  - Code: spin-loop (HLT + JMP .-1) so QEMU doesn't crash

Usage: python3 gen_stub_kernel.py <output.elf>
"""

import struct
import sys

def u8(v):  return struct.pack('B', v & 0xFF)
def u16(v): return struct.pack('<H', v & 0xFFFF)
def u32(v): return struct.pack('<I', v & 0xFFFFFFFF)
def u64(v): return struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF)

def pad_to(data, size, fill=0):
    if len(data) < size:
        data += bytes([fill]) * (size - len(data))
    return data

# ---- Stub code: HLT loop ----
# In 64-bit mode: HLT (F4) + short JMP -2 (EB FE)
stub_code = bytes([0xF4, 0xEB, 0xFD])   # hlt; jmp $-2

# ---- ELF64 constants ----
LOAD_ADDR   = 0x100000      # Physical and virtual load address (kernel at 1MB)
ENTRY_POINT = LOAD_ADDR

# ELF64 header = 64 bytes
# Program header = 56 bytes
# Total header area = 120 bytes, rounded to 512 for alignment
HDR_SIZE   = 64 + 56        # ELF header + one phdr
CODE_OFFSET = 512           # Code starts at file offset 512 (after 1 sector headers)

# Code size (pad to 512 bytes)
code_data = pad_to(stub_code, 512)

# ---- Build ELF header ----
ehdr = b''
ehdr += b'\x7fELF'          # e_ident magic
ehdr += u8(2)               # EI_CLASS = ELFCLASS64
ehdr += u8(1)               # EI_DATA = little-endian
ehdr += u8(1)               # EI_VERSION = current
ehdr += u8(0)               # EI_OSABI = System V
ehdr += b'\x00' * 8         # EI_ABIVERSION + padding
ehdr += u16(2)              # e_type = ET_EXEC
ehdr += u16(0x3E)           # e_machine = x86-64
ehdr += u32(1)              # e_version = current
ehdr += u64(ENTRY_POINT)    # e_entry
ehdr += u64(64)             # e_phoff = right after ELF header (64 bytes in)
ehdr += u64(0)              # e_shoff = no section headers
ehdr += u32(0)              # e_flags
ehdr += u16(64)             # e_ehsize = ELF header size
ehdr += u16(56)             # e_phentsize = program header entry size
ehdr += u16(1)              # e_phnum = 1 program header
ehdr += u16(64)             # e_shentsize
ehdr += u16(0)              # e_shnum
ehdr += u16(0)              # e_shstrndx

assert len(ehdr) == 64, f"ELF header wrong size: {len(ehdr)}"

# ---- Build program header ----
p_filesz = len(code_data)
p_memsz  = len(code_data)

phdr = b''
phdr += u32(1)              # p_type = PT_LOAD
phdr += u32(5)              # p_flags = PF_R | PF_X (read + execute)
phdr += u64(CODE_OFFSET)    # p_offset = file offset of segment data
phdr += u64(LOAD_ADDR)      # p_vaddr = virtual address
phdr += u64(LOAD_ADDR)      # p_paddr = physical address
phdr += u64(p_filesz)       # p_filesz
phdr += u64(p_memsz)        # p_memsz
phdr += u64(0x1000)         # p_align = 4KB

assert len(phdr) == 56, f"Program header wrong size: {len(phdr)}"

# ---- Assemble ELF file ----
elf  = ehdr
elf += phdr
elf  = pad_to(elf, CODE_OFFSET)  # Pad header area to CODE_OFFSET
elf += code_data

output_path = sys.argv[1] if len(sys.argv) > 1 else 'kernel_stub.elf'
with open(output_path, 'wb') as f:
    f.write(elf)

print(f"Generated stub kernel: {output_path} ({len(elf)} bytes)")
print(f"  Entry point: 0x{ENTRY_POINT:X}")
print(f"  Load addr:   0x{LOAD_ADDR:X}")
print(f"  Code:        HLT+JMP loop (safe halt)")
