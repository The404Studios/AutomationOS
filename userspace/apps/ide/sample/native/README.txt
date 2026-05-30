Native sample programs for the AutomationOS IDE toolchain.

These are SOURCE assets only -- they are NOT compiled at build time.
The IDE compiles each one on-device with its native toolchain via
Build (B), then launches the resulting static ELF via Run (R). Each
program proves the toolchain works by returning a known exit code.

Expected exit codes:
  sum.c   -> 55   (1+2+...+10)
  fact.c  -> 120  (5! = 5*4*3*2*1)
  add.c   -> 42   (add(40, 2), a function call)
  hello.c -> 0    (writes a line via sys_write, then exits)
  add.asm -> 7    (hand-written SYS_EXIT in the Intel-subset assembler)
