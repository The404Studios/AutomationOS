/* WINFIX nocrt_hello -- the BARE MINIMUM Windows program: no C runtime, three kernel32 imports.
 *
 *   x86_64-w64-mingw32-gcc -O2 -nostdlib -ffreestanding -fno-stack-protector \
 *       -Wl,-e,start -Wl,--subsystem,console -Wl,--dynamicbase -Wl,--enable-reloc-section \
 *       -o nocrt_hello.exe nocrt_hello.c -lkernel32
 *
 * Proves, in order, that the loader can: map the image, apply relocations, resolve IAT entries to working
 * functions, call through the Windows x64 ABI, write to the console and exit with a code.
 * Expected output:   "WINFIX: nocrt hello"   and exit code 42.
 */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD id);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code) __attribute__((noreturn));

/* A global with an ADDRESS in its initializer forces a base relocation (DIR64) into the image: if relocations
 * are applied wrongly, `g_msg_ptr` points into the weeds and the program prints garbage / faults. */
static const char g_msg[] = "WINFIX: nocrt hello\n";
static const char *volatile g_msg_ptr = g_msg;     /* volatile: the compiler may not fold the pointer away -- the DIR64 reloc must exist */

void __attribute__((noreturn)) start(void)
{
    DWORD written = 0;
    HANDLE out = GetStdHandle((DWORD)-11);        /* STD_OUTPUT_HANDLE */
    WriteFile(out, g_msg_ptr, (DWORD)(sizeof(g_msg) - 1), &written, 0);
    ExitProcess(written == sizeof(g_msg) - 1 ? 42 : 1);
}
