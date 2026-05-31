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

Games / demos (compile with B, run with R -- each prints to the console
and exits 0; all self-contained, integer-only, no #include, no libc):
  hanoi.c  -> Towers of Hanoi solved recursively (15 moves for 4 disks).
  life.c   -> Conway's Game of Life: a glider + a blinker, six generations.
  mandel.c -> the Mandelbrot set in ASCII, rendered in fixed-point integers
              (the toolchain has no floating point).

These are "zero-player" games: they run a computation and print frames,
because the toolchain does not yet emit the SYS_READ syscall, so a compiled
program cannot read the keyboard. Adding a sys_read() builtin to cc_codegen.c
(mirroring sys_write) is the next step toward interactive games.

The host-side checker test/game_check.c compiles any one of these through the
exact on-device toolchain (no boot needed) and confirms it yields a valid
static ELF -- handy when authoring new programs.
