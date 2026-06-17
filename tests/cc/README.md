# cc regression suite (CC-REGRESSION-SUITE-0)

Permanent correctness suite for the on-device self-hosting C compiler (`cc` → `as` → ELF).
Every codegen bug we fix becomes a permanent case here, so it can never silently regress.

## Why this exists

The flagship capability of this OS is *compiling real C on the device*. A miscompile of global
state (e.g. `int g = 5;` returning `0`) silently undermines everything built on top of it. This
suite turns each such bug into a guarded invariant.

## How it runs

`build_test/cc_regression_smoke.sh` compiles each `*.c` here with the real on-device `cc` under
QEMU, runs the resulting ELF, and checks the **exit code** against the `EXPECT:` value declared in
the file header. (A future fast-path may build the `cc_*.c` pipeline with host gcc for quicker
iteration; the QEMU path is the faithful proof.)

Acceptance line: `CCREGRESSION: PASS tests=NN failures=0`.

## Cases

| File | Expect | Targets | Pre-fix symptom |
|---|---|---|---|
| `global_scalar_init.c` | 5 | B3 CC-GLOBALINIT-0 | global scalar init dropped → returns 0 |
| `global_array_init.c` | 40 | B4 CC-ARRAY-INITLIST-0 | array brace-init dropped / wrong elem size |
| `init_list.c` | 10 | B3+B4 | init-list values zeroed → sum 0 |

### Planned (added as the fixes land)

- `const_table.c` (=8) — `const` qualified global lookup table.
- `array_index.c` (=8) — variable subscript element-size correctness (B4).
- `pointer_arith.c` (=9) — `int *p = a; *(p+2)`.
- `struct_init.c` (=33) — global struct brace-init (stretch; may defer if out of the cc subset).

## Convention

Each test is a minimal program whose `main` returns a known value. Header carries:

```
/* one-line description (brick it targets)
 * EXPECT: <exit code>
 * Pre-fix symptom: <what's wrong today> */
```
