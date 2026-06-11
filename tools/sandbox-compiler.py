#!/usr/bin/env python3
"""
Sandbox Profile Compiler
Compiles human-readable sandbox profiles to BPF bytecode

Usage:
    python sandbox-compiler.py <profile.profile> [-o output.bpf] [--optimize] [--validate]

Output formats:
    - Binary BPF bytecode (.bpf)
    - C array initialization (.c)
    - Assembly listing (.asm)
"""

import sys
import struct
import argparse
import re
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass
from enum import IntEnum

# ========================================
# BPF Instruction Encoding
# ========================================

class BPFClass(IntEnum):
    LD = 0x00
    JMP = 0x05
    RET = 0x06

class BPFSize(IntEnum):
    W = 0x00  # Word (32-bit)
    H = 0x08  # Half-word (16-bit)
    B = 0x10  # Byte (8-bit)

class BPFMode(IntEnum):
    IMM = 0x00  # Immediate
    ABS = 0x20  # Absolute

class BPFOp(IntEnum):
    JEQ = 0x10
    JGT = 0x20
    JGE = 0x30
    JSET = 0x40

class BPFSrc(IntEnum):
    K = 0x00  # Constant
    X = 0x08  # X register

# Seccomp actions
SECCOMP_RET_KILL = 0x00000000
SECCOMP_RET_TRAP = 0x00030000
SECCOMP_RET_ERRNO = 0x00050000
SECCOMP_RET_TRACE = 0x7ff00000
SECCOMP_RET_LOG = 0x7ffc0000
SECCOMP_RET_ALLOW = 0x7fff0000

# Syscall offsets in seccomp_data
OFFSET_NR = 0
OFFSET_ARCH = 4
OFFSET_IP = 8
OFFSET_ARGS = 16

# Architecture constants
AUDIT_ARCH_X86_64 = 0xc000003e

@dataclass
class BPFInsn:
    """BPF instruction"""
    code: int
    jt: int
    jf: int
    k: int

    def encode(self) -> bytes:
        """Encode instruction to binary format"""
        return struct.pack('<HBBI', self.code, self.jt, self.jf, self.k)

    def __str__(self) -> str:
        """Disassemble instruction"""
        cls = self.code & 0x07
        if cls == BPFClass.LD:
            mode = self.code & 0xe0
            size = self.code & 0x18
            size_str = {BPFSize.W: 'w', BPFSize.H: 'h', BPFSize.B: 'b'}.get(size, '?')
            if mode == BPFMode.ABS:
                return f"ld {size_str} [#{self.k}]"
            elif mode == BPFMode.IMM:
                return f"ld #{self.k:#x}"
        elif cls == BPFClass.JMP:
            op = self.code & 0xf0
            op_str = {
                BPFOp.JEQ: 'jeq',
                BPFOp.JGT: 'jgt',
                BPFOp.JGE: 'jge',
                BPFOp.JSET: 'jset'
            }.get(op, 'j?')
            return f"{op_str} #{self.k:#x}, {self.jt}, {self.jf}"
        elif cls == BPFClass.RET:
            return f"ret #{self.k:#x}"
        return f"? code={self.code:#x} k={self.k:#x}"

# ========================================
# Profile Parser
# ========================================

@dataclass
class Rule:
    """Sandbox rule"""
    action: str  # ALLOW, DENY, TRAP, KILL
    syscall: str
    condition: Optional[str] = None
    errno: Optional[int] = None

@dataclass
class Profile:
    """Parsed sandbox profile"""
    name: str
    version: str
    description: str
    arch: str
    default_action: str
    rules: List[Rule]
    capabilities: Dict[str, List[str]]
    rlimits: Dict[str, int]
    namespace_config: Dict[str, str]

# Syscall name to number mapping (AutomationOS syscall numbers)
SYSCALL_MAP = {
    'sys_exit': 0,
    'sys_fork': 1,
    'sys_read': 2,
    'sys_write': 3,
    'sys_open': 4,
    'sys_close': 5,
    'sys_waitpid': 6,
    'sys_execve': 7,
    'sys_getpid': 8,
    'sys_sleep': 9,
    # Add more syscalls as they are implemented
}

# Errno constants
ERRNO_MAP = {
    'EPERM': 1,
    'ENOENT': 2,
    'ESRCH': 3,
    'EINTR': 4,
    'EIO': 5,
    'ENXIO': 6,
    'E2BIG': 7,
    'ENOEXEC': 8,
    'EBADF': 9,
    'ECHILD': 10,
    'EAGAIN': 11,
    'ENOMEM': 12,
    'EACCES': 13,
    'EFAULT': 14,
    'ENOTBLK': 15,
    'EBUSY': 16,
    'EEXIST': 17,
    'EXDEV': 18,
    'ENODEV': 19,
    'ENOTDIR': 20,
    'EISDIR': 21,
    'EINVAL': 22,
    'ENFILE': 23,
    'EMFILE': 24,
    'ENOTTY': 25,
    'ETXTBSY': 26,
    'EFBIG': 27,
    'ENOSPC': 28,
    'ESPIPE': 29,
    'EROFS': 30,
    'ENOSYS': 38,
    'ENETDOWN': 100,
}

def parse_profile(filename: str) -> Profile:
    """Parse sandbox profile file"""
    with open(filename, 'r') as f:
        lines = f.readlines()

    profile = Profile(
        name='',
        version='',
        description='',
        arch='x86_64',
        default_action='KILL',
        rules=[],
        capabilities={'require': [], 'deny': []},
        rlimits={},
        namespace_config={}
    )

    for line in lines:
        line = line.strip()

        # Skip comments and empty lines
        if not line or line.startswith('#'):
            continue

        # Parse metadata
        if line.startswith('NAME:'):
            profile.name = line.split(':', 1)[1].strip()
        elif line.startswith('VERSION:'):
            profile.version = line.split(':', 1)[1].strip()
        elif line.startswith('DESCRIPTION:'):
            profile.description = line.split(':', 1)[1].strip()
        elif line.startswith('ARCH:'):
            profile.arch = line.split(':', 1)[1].strip()
        elif line.startswith('DEFAULT_ACTION:'):
            profile.default_action = line.split(':', 1)[1].strip()

        # Parse rules
        elif line.startswith(('ALLOW ', 'DENY ', 'TRAP ', 'KILL ')):
            parts = line.split(None, 2)
            action = parts[0]
            syscall = parts[1] if len(parts) > 1 else ''

            condition = None
            errno = None

            if len(parts) > 2:
                rest = parts[2]
                # Check for errno
                if action == 'DENY':
                    errno_match = re.search(r'\b([A-Z]+)\b', rest)
                    if errno_match:
                        errno_name = errno_match.group(1)
                        errno = ERRNO_MAP.get(errno_name, 1)
                # Check for condition
                if 'ARG' in rest:
                    condition = rest

            profile.rules.append(Rule(action, syscall, condition, errno))

        # Parse capabilities
        elif line.startswith('CAP_REQUIRE '):
            cap = line.split(None, 1)[1]
            profile.capabilities['require'].append(cap)
        elif line.startswith('CAP_DENY '):
            cap = line.split(None, 1)[1]
            profile.capabilities['deny'].append(cap)

        # Parse resource limits
        elif line.startswith('RLIMIT_'):
            parts = line.split(':', 1)
            if len(parts) == 2:
                key = parts[0].strip()
                value = int(parts[1].strip())
                profile.rlimits[key] = value

        # Parse namespace config
        elif line.startswith('NS_'):
            parts = line.split(':', 1)
            if len(parts) == 2:
                key = parts[0].strip()
                value = parts[1].strip()
                profile.namespace_config[key] = value

    return profile

# ========================================
# BPF Code Generation
# ========================================

class BPFCompiler:
    """Compile profile to BPF bytecode"""

    def __init__(self, profile: Profile):
        self.profile = profile
        self.insns: List[BPFInsn] = []

    def emit(self, code: int, jt: int = 0, jf: int = 0, k: int = 0):
        """Emit BPF instruction"""
        self.insns.append(BPFInsn(code, jt, jf, k))

    def emit_load_nr(self):
        """Load syscall number"""
        self.emit(BPFClass.LD | BPFSize.W | BPFMode.ABS, k=OFFSET_NR)

    def emit_load_arch(self):
        """Load architecture"""
        self.emit(BPFClass.LD | BPFSize.W | BPFMode.ABS, k=OFFSET_ARCH)

    def emit_ret(self, action: int):
        """Return action"""
        self.emit(BPFClass.RET, k=action)

    def emit_jmp_eq(self, k: int, jt: int, jf: int):
        """Jump if equal"""
        self.emit(BPFClass.JMP | BPFOp.JEQ | BPFSrc.K, jt, jf, k)

    def compile(self) -> List[BPFInsn]:
        """Compile profile to BPF instructions"""
        self.insns = []

        # 1. Check architecture (prevent 32-bit syscalls on 64-bit system)
        self.emit_load_arch()
        self.emit_jmp_eq(AUDIT_ARCH_X86_64, 1, 0)
        self.emit_ret(SECCOMP_RET_KILL)

        # 2. Load syscall number
        self.emit_load_nr()

        # 3. Generate rules (most specific first)
        for rule in self.profile.rules:
            if rule.syscall not in SYSCALL_MAP:
                print(f"Warning: Unknown syscall {rule.syscall}, skipping")
                continue

            syscall_nr = SYSCALL_MAP[rule.syscall]

            # Simple rule without conditions
            if not rule.condition:
                # Check if syscall matches
                remaining_insns = 1 if rule.action == 'ALLOW' else 1
                self.emit_jmp_eq(syscall_nr, 0, remaining_insns + 1)

                # Action
                if rule.action == 'ALLOW':
                    self.emit_ret(SECCOMP_RET_ALLOW)
                elif rule.action == 'DENY':
                    errno = rule.errno or 1
                    self.emit_ret(SECCOMP_RET_ERRNO | errno)
                elif rule.action == 'TRAP':
                    self.emit_ret(SECCOMP_RET_TRAP)
                elif rule.action == 'KILL':
                    self.emit_ret(SECCOMP_RET_KILL)

        # 4. Default action
        default_action_map = {
            'ALLOW': SECCOMP_RET_ALLOW,
            'DENY': SECCOMP_RET_ERRNO | 1,
            'TRAP': SECCOMP_RET_TRAP,
            'KILL': SECCOMP_RET_KILL,
        }
        default_action = default_action_map.get(self.profile.default_action, SECCOMP_RET_KILL)
        self.emit_ret(default_action)

        return self.insns

    def optimize(self):
        """Optimize BPF bytecode"""
        # TODO: Implement optimization passes:
        # - Remove redundant loads
        # - Merge consecutive jumps
        # - Reorder rules for better jump prediction
        pass

    def validate(self) -> bool:
        """Validate generated BPF program"""
        if not self.insns:
            print("Error: No instructions generated")
            return False

        # Check program ends with RET
        if (self.insns[-1].code & 0x07) != BPFClass.RET:
            print("Error: Program must end with RET instruction")
            return False

        # Check jump targets
        for i, insn in enumerate(self.insns):
            if (insn.code & 0x07) == BPFClass.JMP:
                jt_target = i + insn.jt + 1
                jf_target = i + insn.jf + 1
                if jt_target >= len(self.insns) or jf_target >= len(self.insns):
                    print(f"Error: Jump out of bounds at instruction {i}")
                    return False

        return True

# ========================================
# Output Formats
# ========================================

def write_binary(insns: List[BPFInsn], filename: str):
    """Write BPF bytecode to binary file"""
    with open(filename, 'wb') as f:
        for insn in insns:
            f.write(insn.encode())
    print(f"Written {len(insns)} instructions to {filename}")

def write_c_array(insns: List[BPFInsn], filename: str, profile_name: str):
    """Write BPF program as C array"""
    with open(filename, 'w') as f:
        f.write(f"// Generated BPF filter for profile: {profile_name}\n")
        f.write(f"// Total instructions: {len(insns)}\n\n")
        f.write(f"static struct bpf_insn filter_{profile_name}[] = {{\n")
        for insn in insns:
            f.write(f"    {{ .code = 0x{insn.code:04x}, .jt = {insn.jt}, .jf = {insn.jf}, .k = 0x{insn.k:08x} }},\n")
        f.write("};\n\n")
        f.write(f"#define FILTER_{profile_name.upper()}_LEN {len(insns)}\n")
    print(f"Written C array to {filename}")

def write_asm(insns: List[BPFInsn], filename: str):
    """Write disassembly listing"""
    with open(filename, 'w') as f:
        f.write("; BPF Disassembly\n\n")
        for i, insn in enumerate(insns):
            f.write(f"{i:4d}: {str(insn)}\n")
    print(f"Written assembly listing to {filename}")

# ========================================
# Main
# ========================================

def main():
    parser = argparse.ArgumentParser(description='Compile sandbox profiles to BPF bytecode')
    parser.add_argument('profile', help='Input profile file (.profile)')
    parser.add_argument('-o', '--output', help='Output file (default: <profile>.bpf)')
    parser.add_argument('--optimize', action='store_true', help='Optimize BPF bytecode')
    parser.add_argument('--validate', action='store_true', help='Validate BPF program')
    parser.add_argument('--format', choices=['binary', 'c', 'asm'], default='binary',
                       help='Output format')
    parser.add_argument('--verbose', action='store_true', help='Verbose output')

    args = parser.parse_args()

    # Parse profile
    print(f"Parsing profile: {args.profile}")
    profile = parse_profile(args.profile)

    if args.verbose:
        print(f"Profile: {profile.name} v{profile.version}")
        print(f"Description: {profile.description}")
        print(f"Rules: {len(profile.rules)}")
        print(f"Default action: {profile.default_action}")

    # Compile to BPF
    print("Compiling to BPF bytecode...")
    compiler = BPFCompiler(profile)
    insns = compiler.compile()
    print(f"Generated {len(insns)} BPF instructions")

    # Optimize
    if args.optimize:
        print("Optimizing...")
        compiler.optimize()
        print(f"Optimized to {len(compiler.insns)} instructions")

    # Validate
    if args.validate or args.verbose:
        print("Validating...")
        if compiler.validate():
            print("✓ Validation passed")
        else:
            print("✗ Validation failed")
            return 1

    # Determine output filename
    if args.output:
        output = args.output
    else:
        base = args.profile.rsplit('.', 1)[0]
        ext = {'binary': '.bpf', 'c': '.c', 'asm': '.asm'}[args.format]
        output = base + ext

    # Write output
    if args.format == 'binary':
        write_binary(insns, output)
    elif args.format == 'c':
        write_c_array(insns, output, profile.name)
    elif args.format == 'asm':
        write_asm(insns, output)

    # Print statistics
    if args.verbose:
        print("\nStatistics:")
        print(f"  Profile name: {profile.name}")
        print(f"  Arch: {profile.arch}")
        print(f"  Rules: {len(profile.rules)}")
        print(f"  BPF instructions: {len(insns)}")
        print(f"  Binary size: {len(insns) * 8} bytes")

    return 0

if __name__ == '__main__':
    sys.exit(main())
