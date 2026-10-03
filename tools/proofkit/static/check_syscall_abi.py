#!/usr/bin/env python3
"""ABI drift guard: every UAPI_SYS_<NAME> in kernel/include/uapi/syscalls.h must equal SYS_<NAME> in
kernel/include/syscall.h.

The uapi header drifted (16 of 48 numbers wrong) and nothing noticed, because nothing includes it -- until new
code trusted it: a test "slept" through the wrong syscall number and ran before the NIC link was up.

  check_syscall_abi.py          exit 0 if consistent, 1 (and a list) if not
  check_syscall_abi.py --fix    rewrite the uapi numbers from the kernel header (the kernel header is the truth)
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
KERNEL_HDR = os.path.join(ROOT, "kernel/include/syscall.h")
UAPI_HDR = os.path.join(ROOT, "kernel/include/uapi/syscalls.h")

KERNEL_DEF = re.compile(r"\s*#define\s+SYS_([A-Z0-9_]+)\s+(\d+)\b")
UAPI_DEF = re.compile(r"(#define\s+UAPI_SYS_)([A-Z0-9_]+)(\s+)(\d+)")


def kernel_numbers():
    nums = {}
    for line in open(KERNEL_HDR, encoding="utf-8", errors="replace"):
        m = KERNEL_DEF.match(line)
        if m:
            nums[m.group(1)] = int(m.group(2))
    return nums


def main():
    truth = kernel_numbers()
    text = open(UAPI_HDR, "rb").read().decode("utf-8", "replace")
    drift = []

    def correct(m):
        prefix, name, space, value = m.group(1), m.group(2), m.group(3), int(m.group(4))
        if name in truth and truth[name] != value:
            drift.append((name, value, truth[name]))
            return "%s%s%s%d" % (prefix, name, space, truth[name])
        return m.group(0)

    fixed = UAPI_DEF.sub(correct, text)
    checked = len(UAPI_DEF.findall(text))

    if "--fix" in sys.argv:
        if drift:
            open(UAPI_HDR, "wb").write(fixed.encode("utf-8"))
        print("fixed %d number(s)" % len(drift))
        return 0

    for name, uapi, kern in drift:
        print("DRIFT  %-14s uapi=%d kernel=%d" % (name, uapi, kern))
    print("syscall ABI: %s (%d uapi names checked against %d kernel names)"
          % ("OK" if not drift else "DRIFTED (%d wrong)" % len(drift), checked, len(truth)))
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
