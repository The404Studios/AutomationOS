#!/usr/bin/env python3
"""
MAC Policy Compiler

Compiles human-readable MAC policy files into binary format for kernel loading.

Policy Language Syntax:
    # Comment
    allow <source_domain> <target_domain>:<object_class> { <permissions> };
    deny <source_domain> <target_domain>:<object_class> { <permissions> };
    transition <source_domain> <target_domain>:<path_pattern> -> <result_domain>;

Example:
    allow user_t home_t:file { read write create delete };
    deny user_t shadow_t:file { read write };
    transition user_t bin_t:/bin/su -> admin_t;
"""

import sys
import struct
import re
from pathlib import Path

# Constants matching kernel definitions
POLICY_MAGIC = 0x4D414350  # "MACP"
POLICY_VERSION = 1

MAX_LABEL_NAME = 64
MAX_PATH_PATTERN = 256

# Object types
OBJ_TYPES = {
    'file': 0,
    'dir': 1,
    'socket': 2,
    'device': 3,
    'process': 4,
    'shm': 5,
    'msg': 6,
    'sem': 7,
}

# File permissions
FILE_PERMS = {
    'read': 1 << 0,
    'write': 1 << 1,
    'execute': 1 << 2,
    'append': 1 << 3,
    'create': 1 << 4,
    'delete': 1 << 5,
    'chown': 1 << 6,
    'chmod': 1 << 7,
}

# Network permissions
NET_PERMS = {
    'bind': 1 << 0,
    'connect': 1 << 1,
    'listen': 1 << 2,
    'accept': 1 << 3,
    'send': 1 << 4,
    'recv': 1 << 5,
    'raw': 1 << 6,
}

# Process permissions
PROC_PERMS = {
    'signal': 1 << 0,
    'ptrace': 1 << 1,
    'kill': 1 << 2,
    'setprio': 1 << 3,
    'fork': 1 << 4,
    'exec': 1 << 5,
    'transition': 1 << 6,
}

# IPC permissions
IPC_PERMS = {
    'read': 1 << 0,
    'write': 1 << 1,
    'create': 1 << 2,
    'destroy': 1 << 3,
    'getattr': 1 << 4,
    'setattr': 1 << 5,
}

# Rule flags
RULE_FLAG_AUDIT = 1 << 0
RULE_FLAG_DENY = 1 << 1

# MLS levels
MLS_LEVELS = {
    'unclassified': 0,
    'confidential': 1,
    'secret': 2,
    'top_secret': 3,
}


class PolicyRule:
    """Represents a MAC policy rule"""

    def __init__(self, source, target, obj_type, permissions, flags=0,
                 min_level=0, max_level=3):
        self.source = source
        self.target = target
        self.obj_type = obj_type
        self.permissions = permissions
        self.flags = flags
        self.min_level = min_level
        self.max_level = max_level

    def to_binary(self):
        """Convert rule to binary format"""
        # struct mac_rule: 2x64 bytes (domains) + 8 bytes (type, perms, levels, flags)
        source_bytes = self.source.encode('utf-8').ljust(MAX_LABEL_NAME, b'\x00')
        target_bytes = self.target.encode('utf-8').ljust(MAX_LABEL_NAME, b'\x00')

        rule_data = struct.pack('<II', self.obj_type, self.permissions)
        rule_data += struct.pack('<II', self.min_level, self.max_level)
        rule_data += struct.pack('<Q', self.flags)

        return source_bytes + target_bytes + rule_data

    def __str__(self):
        rule_type = "deny" if (self.flags & RULE_FLAG_DENY) else "allow"
        return f"{rule_type} {self.source} {self.target}:{self.obj_type} (perms=0x{self.permissions:x})"


class PolicyTransition:
    """Represents a domain transition rule"""

    def __init__(self, source, target, result, path_pattern, flags=0):
        self.source = source
        self.target = target
        self.result = result
        self.path_pattern = path_pattern
        self.flags = flags

    def to_binary(self):
        """Convert transition to binary format"""
        # struct mac_transition: 3x64 bytes (domains) + 256 bytes (path) + 8 bytes (flags)
        source_bytes = self.source.encode('utf-8').ljust(MAX_LABEL_NAME, b'\x00')
        target_bytes = self.target.encode('utf-8').ljust(MAX_LABEL_NAME, b'\x00')
        result_bytes = self.result.encode('utf-8').ljust(MAX_LABEL_NAME, b'\x00')
        path_bytes = self.path_pattern.encode('utf-8').ljust(MAX_PATH_PATTERN, b'\x00')
        flags_bytes = struct.pack('<Q', self.flags)

        return source_bytes + target_bytes + result_bytes + path_bytes + flags_bytes

    def __str__(self):
        return f"transition {self.source} {self.target}:{self.path_pattern} -> {self.result}"


class PolicyCompiler:
    """Compiles text policy files into binary format"""

    def __init__(self):
        self.rules = []
        self.transitions = []
        self.errors = []

    def parse_line(self, line, line_num):
        """Parse a single line of policy"""
        # Remove comments and strip whitespace
        line = line.split('#')[0].strip()
        if not line:
            return

        try:
            if line.startswith('allow') or line.startswith('deny'):
                self.parse_rule(line, line_num)
            elif line.startswith('transition'):
                self.parse_transition(line, line_num)
            else:
                self.errors.append(f"Line {line_num}: Unknown statement: {line}")
        except Exception as e:
            self.errors.append(f"Line {line_num}: Parse error: {str(e)}")

    def parse_rule(self, line, line_num):
        """Parse an allow/deny rule"""
        is_deny = line.startswith('deny')
        line = line[4:] if not is_deny else line[4:]  # Remove "allow" or "deny"
        line = line.strip()

        # Pattern: source target:class { perms };
        match = re.match(r'(\S+)\s+(\S+):(\S+)\s+\{\s*([^}]+)\s*\}\s*;?', line)
        if not match:
            raise ValueError(f"Invalid rule syntax")

        source = match.group(1)
        target = match.group(2)
        obj_class = match.group(3)
        perm_str = match.group(4)

        # Validate domain names
        if not source.endswith('_t'):
            raise ValueError(f"Invalid source domain: {source}")
        if target != '*' and not target.endswith('_t'):
            raise ValueError(f"Invalid target domain: {target}")

        # Get object type
        if obj_class not in OBJ_TYPES:
            raise ValueError(f"Unknown object class: {obj_class}")
        obj_type = OBJ_TYPES[obj_class]

        # Parse permissions
        perm_list = [p.strip() for p in perm_str.split()]
        permissions = 0

        perm_map = FILE_PERMS if obj_class in ['file', 'dir'] else \
                   NET_PERMS if obj_class == 'socket' else \
                   PROC_PERMS if obj_class == 'process' else \
                   IPC_PERMS

        for perm in perm_list:
            if perm not in perm_map:
                raise ValueError(f"Unknown permission '{perm}' for class {obj_class}")
            permissions |= perm_map[perm]

        flags = RULE_FLAG_DENY if is_deny else 0

        rule = PolicyRule(source, target, obj_type, permissions, flags)
        self.rules.append(rule)

    def parse_transition(self, line, line_num):
        """Parse a domain transition rule"""
        # Pattern: transition source target:path -> result;
        match = re.match(r'transition\s+(\S+)\s+(\S+):(\S+)\s+->\s+(\S+)\s*;?', line)
        if not match:
            raise ValueError(f"Invalid transition syntax")

        source = match.group(1)
        target = match.group(2)
        path = match.group(3)
        result = match.group(4)

        # Validate domains
        if not source.endswith('_t'):
            raise ValueError(f"Invalid source domain: {source}")
        if not target.endswith('_t'):
            raise ValueError(f"Invalid target domain: {target}")
        if not result.endswith('_t'):
            raise ValueError(f"Invalid result domain: {result}")

        trans = PolicyTransition(source, target, result, path)
        self.transitions.append(trans)

    def compile_file(self, input_path):
        """Compile a policy file"""
        print(f"Compiling policy: {input_path}")

        with open(input_path, 'r') as f:
            for line_num, line in enumerate(f, 1):
                self.parse_line(line, line_num)

        if self.errors:
            print(f"\nErrors found:")
            for error in self.errors:
                print(f"  {error}")
            return False

        print(f"\nSuccessfully parsed:")
        print(f"  {len(self.rules)} rules")
        print(f"  {len(self.transitions)} transitions")

        return True

    def generate_binary(self, output_path):
        """Generate binary policy file"""
        print(f"\nGenerating binary policy: {output_path}")

        # Policy header
        header = struct.pack('<IIIIII',
                           POLICY_MAGIC,
                           POLICY_VERSION,
                           len(self.rules),
                           len(self.transitions),
                           0,  # flags
                           0)  # reserved

        # Serialize rules
        rules_data = b''.join(rule.to_binary() for rule in self.rules)

        # Serialize transitions
        trans_data = b''.join(trans.to_binary() for trans in self.transitions)

        # Write to file
        with open(output_path, 'wb') as f:
            f.write(header)
            f.write(b'\x00' * 8)  # reserved[3]
            f.write(rules_data)
            f.write(trans_data)

        file_size = len(header) + 8 + len(rules_data) + len(trans_data)
        print(f"Binary policy generated: {file_size} bytes")

    def print_summary(self):
        """Print policy summary"""
        print("\n" + "="*60)
        print(" Policy Summary")
        print("="*60)

        print("\nRules:")
        for rule in self.rules:
            print(f"  {rule}")

        print("\nTransitions:")
        for trans in self.transitions:
            print(f"  {trans}")

        print("="*60)


def main():
    if len(sys.argv) < 2:
        print("Usage: policy_compiler.py <input.policy> [output.bin]")
        print("\nCompiles human-readable MAC policy files into binary format.")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else input_path.replace('.policy', '.bin')

    compiler = PolicyCompiler()

    if not compiler.compile_file(input_path):
        print("\nCompilation failed!")
        sys.exit(1)

    compiler.print_summary()
    compiler.generate_binary(output_path)

    print("\n✓ Compilation successful!")

if __name__ == "__main__":
    main()
