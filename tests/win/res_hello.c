/* res_hello.c -- minimal no-CRT Windows x64 program carrying the resources of res_good.rc.
 * Built by tests/win/run_res_tests.sh (never executed by the tests; only its PE structure is read):
 *   x86_64-w64-mingw32-windres -O coff -I tests/win -i tests/win/res_good.rc -o res_good.res.o
 *   x86_64-w64-mingw32-gcc -O2 -nostdlib -ffreestanding -fno-stack-protector -Wl,-e,start -Wl,--subsystem,console \
 *       -Wl,--dynamicbase -Wl,--enable-reloc-section -o res_hello.exe res_hello.c res_good.res.o -lkernel32
 */
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code) __attribute__((noreturn));

void __attribute__((noreturn)) start(void)
{
    ExitProcess(0);
}
