#!/bin/bash
# Build compositor + the full userspace app suite, package into the initrd,
# rebuild the GRUB ISO. Run: wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/build_all.sh'
set -e
cd "$(dirname "$0")/.."

# -mstackrealign: the ELF entry _start is a plain C function but the kernel
# enters it with RSP 16-aligned (via IRETQ), while GCC compiles every function
# assuming the post-call ABI (RSP%16==8). That 8-byte mismatch makes any aligned
# SSE store (movaps) to a stack slot #GP. -mstackrealign makes each function
# realign its own stack, eliminating the whole class of crash.
CF="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2"
LD="ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld"
cc() { gcc $CF -c "$1" -o "$2"; }

# STRESS=1 adds -DPREEMPT_STRESS to the init compile ONLY, so init spawns the
# never-yielding cpuburners (see the #ifdef PREEMPT_STRESS block in
# userspace/init/main.c). With STRESS unset, INIT_EXTRA is empty and the default
# cooperative init is built byte-for-byte as before (no burners ever spawned).
INIT_EXTRA=""
if [ "${STRESS:-0}" = "1" ]; then
    INIT_EXTRA="$INIT_EXTRA -DPREEMPT_STRESS"
    echo "*** STRESS build: init compiled with -DPREEMPT_STRESS (spawns cpuburners) ***"
fi
# GAMETEST=1 adds -DGAMETEST_RUN to the init compile ONLY, so init spawns
# sbin/gametest (the spawn+survive harness over all games + key apps). Unset =>
# the normal boot is byte-for-byte unchanged.
if [ "${IDE:-0}" = "1" ]; then
    INIT_EXTRA="$INIT_EXTRA -DIDE_AUTOSTART"
    echo "*** IDE build: init compiled with -DIDE_AUTOSTART (auto-opens sbin/ide) ***"
fi
if [ "${GAMETEST:-0}" = "1" ]; then
    INIT_EXTRA="$INIT_EXTRA -DGAMETEST_RUN"
    echo "*** GAMETEST build: init compiled with -DGAMETEST_RUN (spawns sbin/gametest) ***"
fi

echo "[all] shared libs..."
cc userspace/lib/font/bitfont.c /tmp/bf.o
cc userspace/lib/font2/font2.c /tmp/font2.o   # scaled (2x) text renderer for IDE/chrome
cc userspace/lib/wl/wl_client.c /tmp/wlc.o
# g3d: software 3D rasterizer (Q16.16 fixed-point vec3/mat4 + z-buffer triangle
# fill). No GPU; renders into the wl window's ARGB32 buffer. Used by cube3d + ray.
cc userspace/lib/g3d/g3d.c      /tmp/g3d.o
cc userspace/lib/ui/ui.c        /tmp/ui.o
cc userspace/lib/keymap/keymap.c /tmp/keymap.o
cc userspace/lib/game/game.c    /tmp/game.o
cc userspace/lib/aictl/aictl.c  /tmp/aictl.o
cc userspace/lib/audio/audio.c  /tmp/audio.o
cc userspace/lib/icon/icon.c    /tmp/icon.o

echo "[all] compositor (m8: right-side dock w/ hover-magnify, folders, bounce) + init..."
cc userspace/compositor/compositor_m8.c /tmp/cm6.o
$LD /tmp/cm6.o /tmp/bf.o /tmp/font2.o /tmp/icon.o -o /tmp/comp.elf
gcc $CF $INIT_EXTRA -c userspace/init/main.c -o /tmp/init.o
$LD /tmp/init.o -o /tmp/init.elf
# forktest: standalone fork/CoW correctness probe (no libs), spawned by init.
cc userspace/apps/forktest/forktest.c /tmp/forktest.o
$LD /tmp/forktest.o -o /tmp/forktest.elf
# threadtest: standalone REAL-THREADS probe (no libs), spawned by init. Proves
# shared address space + independent stacks + independent FPU across 4 threads.
cc userspace/apps/threadtest/threadtest.c /tmp/threadtest.o
$LD /tmp/threadtest.o -o /tmp/threadtest.elf

# AI-native layer + standard tools (self-contained freestanding apps, no libs).
# aibroker = the capability-gated AI command broker (/sbin); the rest are /bin
# tools the shell can spawn (sed/awk/tar/pkg/make/meminfo). Each has a self-test.
echo "[all] AI-native layer + standard tools..."
# crt0: reads the kernel-provided argc/argv off the stack and calls main().
nasm -f elf64 userspace/crt0.asm -o /tmp/crt0.o
# aibroker (reads commands from a file) + meminfo (no args) keep their own _start.
cc userspace/apps/aibroker/aibroker.c /tmp/aibroker.o; $LD /tmp/aibroker.o -o /tmp/aibroker.elf
cc userspace/apps/meminfo/meminfo.c   /tmp/meminfo.o;  $LD /tmp/meminfo.o  -o /tmp/meminfo.elf
# argv-aware tools: link crt0 first (it provides _start -> main(argc,argv)).
cc userspace/apps/sed/sed.c     /tmp/sed.o;     $LD /tmp/crt0.o /tmp/sed.o     -o /tmp/sed.elf
cc userspace/apps/awk/awk.c     /tmp/awk.o;     $LD /tmp/crt0.o /tmp/awk.o     -o /tmp/awk.elf
cc userspace/apps/tar/tar.c     /tmp/tar.o;     $LD /tmp/crt0.o /tmp/tar.o     -o /tmp/tar.elf
cc userspace/apps/pkg/pkg.c     /tmp/pkg.o;     $LD /tmp/crt0.o /tmp/pkg.o     -o /tmp/pkg.elf
cc userspace/apps/make/make.c   /tmp/make.o;    $LD /tmp/crt0.o /tmp/make.o    -o /tmp/make.elf
cc userspace/apps/argvtest/argvtest.c /tmp/argvtest.o; $LD /tmp/crt0.o /tmp/argvtest.o -o /tmp/argvtest.elf
# floattest: proves ring-3 float/SSE at runtime (scalar + 2x2 matmul + reduction).
cc userspace/apps/floattest/floattest.c /tmp/floattest.o; $LD /tmp/crt0.o /tmp/floattest.o -o /tmp/floattest.elf
# sleeptest: proves SYS_SLEEP is a real, ms-granularity, BLOCKING sleep (measures
# elapsed ms across a 50 ms sleep via SYS_GET_TICKS_MS). crt0-linked.
cc userspace/apps/sleeptest/sleeptest.c /tmp/sleeptest.o; $LD /tmp/crt0.o /tmp/sleeptest.o -o /tmp/sleeptest.elf
# cpuburn: never-yielding CPU burner (int + float/SSE) for the preemptive-scheduler
# stress test. Harmless to build/ship unconditionally -- it is only ever SPAWNED
# when init is built with -DPREEMPT_STRESS (STRESS=1). crt0-linked (id from argv[1]).
cc userspace/apps/cpuburn/cpuburn.c /tmp/cpuburn.o; $LD /tmp/crt0.o /tmp/cpuburn.o -o /tmp/cpuburn.elf
# prioritytest: PROVES scheduler priority classes shift CPU share. Forks two
# CPU-bound children (one SCHED_CLASS_HIGH nice -10, one SCHED_CLASS_BACKGROUND
# nice +10), runs them a ~1.5s window with periodic yields, then compares their
# per-process cpu_ticks via SYS_PROCLIST. Prints PRIORITYTEST: PASS iff high >
# low*1.3. Works in BOTH the cooperative and preemptive builds. crt0-linked;
# includes userspace/lib/sched_class.h (named classes + sched_setclass()).
cc userspace/apps/prioritytest/prioritytest.c /tmp/prioritytest.o; $LD /tmp/crt0.o /tmp/prioritytest.o -o /tmp/prioritytest.elf
# gametest: spawn+survive harness over all 16 games + key desktop apps (verifies
# each actually runs its init+render loop without crashing). init spawns it only
# under -DGAMETEST_RUN (GAMETEST=1). crt0-linked.
cc userspace/apps/gametest/gametest.c /tmp/gametest.o; $LD /tmp/crt0.o /tmp/gametest.o -o /tmp/gametest.elf
# matbench: SIMD float matmul benchmark -- scalar baseline vs hand-vectorized SSE
# (gcc v4sf), with a correctness check + a measured speedup. First tensor-runtime brick.
cc userspace/apps/matbench/matbench.c /tmp/matbench.o; $LD /tmp/crt0.o /tmp/matbench.o -o /tmp/matbench.elf
# tensor: freestanding SSE float tensor-kernel lib (matmul/add/scale/relu/dot, each
# 4-wide v4sf + scalar remainder). tensortest runs each SSE op vs an independent
# scalar reference within epsilon and prints TENSORTEST: PASS. crt0-linked.
cc userspace/lib/tensor/tensor.c /tmp/tensor.o
cc userspace/apps/tensortest/tensortest.c /tmp/tensortest.o; $LD /tmp/crt0.o /tmp/tensortest.o /tmp/tensor.o -o /tmp/tensortest.elf
# matmuljobs: a matmul run THROUGH the userspace job queue (lib/jobs) -- work is
# partitioned into row-band jobs, dispatched to 2 worker threads on shared memory,
# drained, and verified bit-identical to the scalar reference. NOT a speedup
# (single-core); it proves the job-coordination machinery is correct. Bare _start
# (no crt0), links the jobs queue + the tensor kernels.
cc userspace/lib/jobs/jobs.c /tmp/jobs.o
cc userspace/apps/matmuljobs/matmuljobs.c /tmp/matmuljobs.o; $LD /tmp/matmuljobs.o /tmp/jobs.o /tmp/tensor.o -o /tmp/matmuljobs.elf

# Wave: process/disk sysutils + file/text tools (all argv-aware, crt0-linked).
# ps/kill/free/uptime = process+system info; find/diff/cmp/tee/wcx/xargs = file
# ops; blk = raw AHCI block read/write. Each self-tests on argc<=1.
cc userspace/apps/blk/blk.c       /tmp/blk.o;     $LD /tmp/crt0.o /tmp/blk.o     -o /tmp/blk.elf
cc userspace/apps/ps/ps.c         /tmp/ps.o;      $LD /tmp/crt0.o /tmp/ps.o      -o /tmp/ps.elf
cc userspace/apps/kill/kill.c     /tmp/kill.o;    $LD /tmp/crt0.o /tmp/kill.o    -o /tmp/kill.elf
cc userspace/apps/free/free.c     /tmp/free.o;    $LD /tmp/crt0.o /tmp/free.o    -o /tmp/free.elf
cc userspace/apps/uptime/uptime.c /tmp/uptime.o;  $LD /tmp/crt0.o /tmp/uptime.o  -o /tmp/uptime.elf
cc userspace/apps/find/find.c     /tmp/find.o;    $LD /tmp/crt0.o /tmp/find.o    -o /tmp/find.elf
cc userspace/apps/diff/diff.c     /tmp/diff.o;    $LD /tmp/crt0.o /tmp/diff.o    -o /tmp/diff.elf
cc userspace/apps/cmp/cmp.c       /tmp/cmp.o;     $LD /tmp/crt0.o /tmp/cmp.o     -o /tmp/cmp.elf
cc userspace/apps/tee/tee.c       /tmp/tee.o;     $LD /tmp/crt0.o /tmp/tee.o     -o /tmp/tee.elf
cc userspace/apps/wcx/wcx.c       /tmp/wcx.o;     $LD /tmp/crt0.o /tmp/wcx.o     -o /tmp/wcx.elf
cc userspace/apps/xargs/xargs.c   /tmp/xargs.o;   $LD /tmp/crt0.o /tmp/xargs.o   -o /tmp/xargs.elf
# gzip/gunzip: the DEFLATE codec lives in its own object, linked alongside.
cc userspace/lib/deflate/deflate.c /tmp/deflate.o
cc userspace/apps/gzip/gzip.c     /tmp/gzip.o;    $LD /tmp/crt0.o /tmp/gzip.o /tmp/deflate.o -o /tmp/gzip.elf
# nettest: networking probe (its own _start, does its own arg-less work) -- queries
# SYS_NET_INFO (MAC/IP), TXes a broadcast ARP, and polls RX for the reply.
cc userspace/apps/nettest/nettest.c /tmp/nettest.o; $LD /tmp/nettest.o -o /tmp/nettest.elf
# sockettest: userspace BSD-socket probe (UDP sendto + bounded recv + TCP alloc).
cc userspace/apps/sockettest/sockettest.c /tmp/sockettest.o; $LD /tmp/sockettest.o -o /tmp/sockettest.elf
# cpu1offload: userspace -> CPU1 matmul offload probe (the userspace coprocessor
# bridge). On the SMP kernel it offloads an int matmul to CPU1 via SYS_CPU1_OFFLOAD
# and prints "CPU1OFFLOAD: PASS ... by_apic=1"; on the DEFAULT kernel the syscall
# is unregistered, so it prints "CPU1OFFLOAD: SKIP" and exits cleanly (harmless).
# Bare _start (no crt0), like nettest/sockettest.
cc userspace/apps/cpu1offload/cpu1offload.c /tmp/cpu1offload.o; $LD /tmp/cpu1offload.o -o /tmp/cpu1offload.elf
# Overhaul-syscall verification probes (bare _start; exercise futex/epoll/
# sendfile/perf/batch against the kernel's real ABI). Each prints "<NAME>: PASS".
for t in futextest epolltest sendfiletest perftest batchtest; do
    cc userspace/apps/$t/$t.c /tmp/$t.o
    $LD /tmp/$t.o -o /tmp/$t.elf
done
# ============================================================================
#  Crypto / TLS / HTTPS stack -- compile ALL library objects FIRST, then link
#  the apps that depend on them (http -> tlsconn -> tls -> crypto).
# ============================================================================
echo "[all] crypto + TLS + net libs..."
# crypto primitives
cc userspace/lib/crypto/sha256.c           /tmp/sha256.o
cc userspace/lib/crypto/sha1.c             /tmp/sha1.o
cc userspace/lib/crypto/md5.c              /tmp/md5.o
cc userspace/lib/crypto/hmac.c             /tmp/hmac.o
cc userspace/lib/crypto/aes.c              /tmp/aes.o
cc userspace/lib/crypto/cryptotest.c       /tmp/cryptokat.o
cc userspace/lib/crypto/bignum.c           /tmp/bignum.o
cc userspace/lib/crypto/rsa.c              /tmp/rsa.o
cc userspace/lib/crypto/sha512.c           /tmp/sha512.o
cc userspace/lib/crypto/hkdf.c             /tmp/hkdf.o
cc userspace/lib/crypto/chacha20poly1305.c /tmp/chacha.o
cc userspace/lib/crypto/x25519.c           /tmp/x25519.o
cc userspace/lib/crypto/p256.c             /tmp/p256.o
cc userspace/lib/crypto/base64.c           /tmp/base64.o
# TLS + PKI
cc userspace/lib/tls/asn1.c                /tmp/asn1.o
cc userspace/lib/tls/x509.c                /tmp/x509.o
cc userspace/lib/tls/x509_verify.c         /tmp/x509_verify.o
cc userspace/lib/tls/ca_bundle.c           /tmp/ca_bundle.o
cc userspace/lib/tls/tls.c                 /tmp/tls.o
# net libs: DNS, HTTP(S), and the plain/TLS transport abstraction
cc userspace/lib/net/dns.c                 /tmp/dns.o
cc userspace/lib/net/http.c                /tmp/http.o
cc userspace/lib/net/tlsconn.c             /tmp/tlsconn.o
# libc string safety net (GCC may emit memcpy/memset under -O2 even w/ -fno-builtin)
cc userspace/libc/string.c                 /tmp/lstring.o
# The full crypto+TLS object bundle (linked into anything that does TLS).
CRYPTO_OBJS="/tmp/sha256.o /tmp/sha1.o /tmp/md5.o /tmp/hmac.o /tmp/aes.o /tmp/cryptokat.o /tmp/bignum.o /tmp/rsa.o /tmp/sha512.o /tmp/hkdf.o /tmp/chacha.o /tmp/x25519.o /tmp/p256.o /tmp/base64.o /tmp/asn1.o /tmp/x509.o /tmp/x509_verify.o /tmp/ca_bundle.o /tmp/tls.o /tmp/lstring.o"
# Everything needed to do http:// + https:// from an app.
HTTPS_OBJS="/tmp/http.o /tmp/dns.o /tmp/deflate.o /tmp/tlsconn.o $CRYPTO_OBJS"

# wget: HTTP/HTTPS download tool (crt0+main; links the full HTTPS stack).
cc userspace/apps/wget/wget.c /tmp/wget.o; $LD /tmp/crt0.o /tmp/wget.o $HTTPS_OBJS -o /tmp/wget.elf
# cryptotest app: runs the full crypto/TLS KAT battery at boot (bare _start -- no crt0).
cc userspace/apps/cryptotest/cryptotest.c /tmp/cryptotest.o
$LD /tmp/cryptotest.o $CRYPTO_OBJS -o /tmp/cryptotest.elf
# tlsprobe: open a real TLS connection to host:443 + report (crt0+main).
cc userspace/apps/tlsprobe/tlsprobe.c /tmp/tlsprobe.o; $LD /tmp/crt0.o /tmp/tlsprobe.o $HTTPS_OBJS -o /tmp/tlsprobe.elf
# certtool: inspect a DER/PEM cert (crt0+main; needs x509 + base64).
cc userspace/apps/certtool/certtool.c /tmp/certtool.o; $LD /tmp/crt0.o /tmp/certtool.o /tmp/x509.o /tmp/asn1.o /tmp/base64.o /tmp/rsa.o /tmp/bignum.o /tmp/lstring.o -o /tmp/certtool.elf

# misc libs + libtest app (json parser + dhcp client + image codec KATs).
cc userspace/lib/json/json.c        /tmp/json.o
cc userspace/lib/net/dhcp.c         /tmp/dhcp.o
cc userspace/lib/imgcodec/bmp.c     /tmp/img_bmp.o
cc userspace/lib/imgcodec/png.c     /tmp/img_png.o
cc userspace/lib/imgcodec/gif.c     /tmp/img_gif.o
cc userspace/lib/imgcodec/imgcodec.c /tmp/img_codec.o
cc userspace/apps/libtest/libtest.c /tmp/libtest.o
$LD /tmp/libtest.o /tmp/json.o /tmp/dhcp.o \
    /tmp/img_bmp.o /tmp/img_png.o /tmp/img_gif.o /tmp/img_codec.o /tmp/deflate.o /tmp/lstring.o -o /tmp/libtest.elf
# dhcpc: obtain + print a DHCP lease (crt0+main; links dhcp).
cc userspace/apps/dhcpc/dhcpc.c /tmp/dhcpc.o; $LD /tmp/crt0.o /tmp/dhcpc.o /tmp/dhcp.o -o /tmp/dhcpc.elf
# apidemo: fetch http(s) URL + pretty-print JSON (crt0+main; HTTPS + json).
cc userspace/apps/apidemo/apidemo.c /tmp/apidemo.o; $LD /tmp/crt0.o /tmp/apidemo.o /tmp/json.o $HTTPS_OBJS -o /tmp/apidemo.elf

# ---- JavaScript engine (from-scratch ES5-subset interpreter) ----
echo "[all] JavaScript engine..."
cc userspace/lib/js/js_lex.c     /tmp/js_lex.o
cc userspace/lib/js/js_parse.c   /tmp/js_parse.o
cc userspace/lib/js/js_value.c   /tmp/js_value.o
cc userspace/lib/js/js_interp.c  /tmp/js_interp.o
cc userspace/lib/js/js_builtin.c /tmp/js_builtin.o
# js_native.o: the native-object bridge. js_value.c now references
# js_native_dispatch_get/_set, so it's a mandatory part of every JS binary.
cc userspace/lib/js/js_native.c  /tmp/js_native.o
JS_OBJS="/tmp/js_lex.o /tmp/js_parse.o /tmp/js_value.o /tmp/js_interp.o /tmp/js_builtin.o /tmp/js_native.o"
cc userspace/apps/js/js.c /tmp/js.o; $LD /tmp/crt0.o /tmp/js.o $JS_OBJS /tmp/lstring.o -o /tmp/js.elf

# ----------------------------------------------------------------------------
# Browser wave (22 agents): DOM + HTML + CSS + Layout + JS web APIs +
# per-layer selftests + new DOM-rendering browser (browser2). All files are
# disjoint additions over the existing tree -- the legacy `browser` stays.
# ----------------------------------------------------------------------------
echo "[all] browser wave: DOM/HTML/CSS/Layout libs + selftests..."
cc userspace/libc/malloc.c              /tmp/lmalloc.o
cc userspace/libc/syscall.c             /tmp/lsyscall.o
cc userspace/lib/dom/dom.c              /tmp/dom.o
cc userspace/lib/dom/dom_selector.c     /tmp/dom_selector.o
cc userspace/lib/dom/dom_event.c        /tmp/dom_event.o
cc userspace/lib/dom/dom_serialize.c    /tmp/dom_serialize.o
cc userspace/lib/dom/dom_util.c         /tmp/dom_util.o
cc userspace/lib/html/html_parse.c      /tmp/html_parse.o
cc userspace/lib/css/css.c              /tmp/css.o
cc userspace/lib/layout/layout.c        /tmp/layout.o
cc userspace/lib/dom/dom_bindings.c     /tmp/dom_bindings.o
cc userspace/lib/js/js_timers.c         /tmp/js_timers.o
cc userspace/lib/js/js_fetch.c          /tmp/js_fetch.o
cc userspace/lib/js/js_storage.c        /tmp/js_storage.o
cc userspace/lib/js/js_console.c        /tmp/js_console.o
cc userspace/lib/js/js_url.c            /tmp/js_url.o

# lstring.o is intentionally excluded -- it lives in CRYPTO_OBJS already, so
# any binary that pulls $HTTPS_OBJS would otherwise get a multiple-definition
# error. The four small selftests below add it explicitly.
DOM_OBJS="/tmp/dom.o /tmp/dom_selector.o /tmp/dom_event.o /tmp/dom_serialize.o /tmp/dom_util.o /tmp/lmalloc.o /tmp/lsyscall.o"
WEB_OBJS="$DOM_OBJS /tmp/html_parse.o /tmp/css.o /tmp/layout.o"
JS_WEB_OBJS="/tmp/dom_bindings.o /tmp/js_timers.o /tmp/js_fetch.o /tmp/js_storage.o /tmp/js_console.o /tmp/js_url.o"

# Per-layer selftest apps (each links only the layers it tests; print PASS/FAIL + exit).
# Each test app gets the full $WEB_OBJS bundle: parsers/css/layout selftests pull
# each other's symbols transitively (e.g. layout_selftest builds a tree via
# html_parse), so linking the whole web stack is simpler than per-app trimming.
cc userspace/apps/domtest/domtest.c       /tmp/domtest.o;    $LD /tmp/crt0.o /tmp/domtest.o    $DOM_OBJS /tmp/lstring.o -o /tmp/domtest.elf
cc userspace/apps/htmltest/htmltest.c     /tmp/htmltest.o;   $LD /tmp/crt0.o /tmp/htmltest.o   $WEB_OBJS /tmp/lstring.o -o /tmp/htmltest.elf
cc userspace/apps/csstest/csstest.c       /tmp/csstest.o;    $LD /tmp/crt0.o /tmp/csstest.o    $WEB_OBJS /tmp/lstring.o -o /tmp/csstest.elf
cc userspace/apps/layouttest/layouttest.c /tmp/layouttest.o; $LD /tmp/crt0.o /tmp/layouttest.o $WEB_OBJS /tmp/lstring.o -o /tmp/layouttest.elf
# webtest: pure end-to-end pipeline KAT (HTML -> DOM -> CSS -> JS mutation -> layout).
# Includes HTTPS_OBJS because JS_WEB_OBJS pulls js_fetch.o which references
# http_get/https_get; the test itself doesn't touch the network.
cc userspace/apps/webtest/webtest.c       /tmp/webtest.o
$LD /tmp/crt0.o /tmp/webtest.o $WEB_OBJS $JS_OBJS $JS_WEB_OBJS $HTTPS_OBJS -o /tmp/webtest.elf
# browser2: GUI browser that renders DOM + runs <script> tags (full pipeline + HTTPS + wl/font).
# browser2_ui.o / browser2_anim.o: the chrome/toolbar UI + tab-strip animation
# helpers (split out so the render core stays focused); both link into browser2.
cc userspace/apps/browser2/browser2.c       /tmp/browser2.o
cc userspace/apps/browser2/browser2_ui.c    /tmp/browser2_ui.o
cc userspace/apps/browser2/browser2_anim.c  /tmp/browser2_anim.o
$LD /tmp/crt0.o /tmp/browser2.o $WEB_OBJS $JS_OBJS $JS_WEB_OBJS $HTTPS_OBJS /tmp/wlc.o /tmp/bf.o /tmp/browser2_ui.o /tmp/browser2_anim.o -o /tmp/browser2.elf
# webapitest: pure JS web-API selftest (timers/fetch/storage/console/url). Links
# the same web+JS+HTTPS object set as webtest (js_fetch.o references http/https).
cc userspace/apps/webapitest/webapitest.c /tmp/webapitest.o
$LD /tmp/crt0.o /tmp/webapitest.o $WEB_OBJS $JS_OBJS $JS_WEB_OBJS $HTTPS_OBJS -o /tmp/webapitest.elf

# ---- Networking tools (link the dns lib) ----
echo "[all] net tools (ping/nc)..."
cc userspace/apps/ping/ping.c /tmp/ping.o; $LD /tmp/crt0.o /tmp/ping.o /tmp/dns.o -o /tmp/ping.elf
cc userspace/apps/nc/nc.c     /tmp/nc.o;   $LD /tmp/crt0.o /tmp/nc.o   /tmp/dns.o -o /tmp/nc.elf

# ---- coreutils expansion + system info (argv-aware, crt0-linked) ----
echo "[all] coreutils expansion..."
for t in grep head tail sort uniq cut tr nl du touch basename dirname uname hostname whoami date less hexdump; do
    cc userspace/apps/$t/$t.c /tmp/$t.o
    $LD /tmp/crt0.o /tmp/$t.o -o /tmp/$t.elf
done

# Toolkit apps: <app> + ui + wl + bitfont
build_ui_app() {   # $1=src $2=name
    cc "$1" /tmp/$2.o
    $LD /tmp/$2.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/$2.elf
}
# wl-direct apps: <app> + wl + bitfont
build_wl_app() {   # $1=src $2=name
    cc "$1" /tmp/$2.o
    $LD /tmp/$2.o /tmp/wlc.o /tmp/bf.o -o /tmp/$2.elf
}
# game-framework apps: <app> + game + wl + bitfont
build_game_app() { # $1=src $2=name
    cc "$1" /tmp/$2.o
    $LD /tmp/$2.o /tmp/game.o /tmp/wlc.o /tmp/bf.o -o /tmp/$2.elf
}

echo "[all] toolkit apps..."
build_ui_app userspace/apps/filemanager/filemanager.c filemanager
build_ui_app userspace/apps/calculator/calculator.c   calculator
build_ui_app userspace/apps/clock/clock.c             clock
build_ui_app userspace/apps/sysinfo/sysinfo.c         sysinfo
build_ui_app userspace/apps/settings/settings.c       settings
build_ui_app userspace/apps/sysmon/sysmon.c           sysmon
build_ui_app userspace/apps/uidemo/uidemo.c           uidemo
build_ui_app userspace/apps/dateapp/dateapp.c         dateapp
build_ui_app userspace/apps/applauncher/applauncher.c applauncher
build_ui_app userspace/apps/taskman/taskman.c         taskman
build_ui_app userspace/apps/startmenu/startmenu.c     startmenu
build_ui_app userspace/apps/controlcenter/controlcenter.c controlcenter

echo "[all] network apps (netman + browser)..."
# netman: network manager (ui toolkit + dns lib). Links like a ui app + dns.o.
cc userspace/apps/netman/netman.c /tmp/netman.o
$LD /tmp/netman.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o /tmp/dns.o -o /tmp/netman.elf
# browser: wl-direct web browser (HTTP + HTTPS + simplified HTML render). Links
# the full HTTPS stack (http+dns+deflate+tlsconn+crypto/tls) + wl+bf.
cc userspace/apps/browser/browser.c /tmp/browser.o
$LD /tmp/browser.o $HTTPS_OBJS /tmp/wlc.o /tmp/bf.o -o /tmp/browser.elf

echo "[all] wl-direct apps..."
# terminal: terminal_m3 + sh_git (native git-like VCS) + wl + bitfont
cc userspace/apps/terminal/terminal_m3.c /tmp/terminal.o
cc userspace/apps/terminal/sh_git.c      /tmp/sh_git.o
$LD /tmp/terminal.o /tmp/sh_git.o /tmp/wlc.o /tmp/bf.o /tmp/keymap.o -o /tmp/terminal.elf
build_wl_app userspace/apps/editor/editor.c           editor
build_wl_app userspace/apps/snake/snake.c             snake
build_wl_app userspace/apps/asteroids/asteroids.c     asteroids
# cube3d / ray: software 3D (g3d lib). They need wl + bitfont + g3d, so they do
# NOT fit build_wl_app (which omits g3d.o) -- link explicitly.
cc userspace/apps/cube3d/cube3d.c /tmp/cube3d.o
$LD /tmp/cube3d.o /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/cube3d.elf
cc userspace/apps/ray/ray.c       /tmp/ray.o
$LD /tmp/ray.o    /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/ray.elf
build_wl_app userspace/apps/sudoku/sudoku.c           sudoku
build_wl_app userspace/apps/pacman/pacman.c           pacman
build_wl_app userspace/apps/clockapp/clockapp.c       clockapp
# photos: a Windows-style image viewer; links the image codec (PNG/BMP/GIF +
# deflate) objects built in the libtest section, so it needs a custom link.
cc userspace/apps/photos/photos.c /tmp/photos.o
$LD /tmp/photos.o /tmp/wlc.o /tmp/bf.o /tmp/img_codec.o /tmp/img_png.o /tmp/img_bmp.o /tmp/img_gif.o /tmp/deflate.o /tmp/lstring.o -o /tmp/photos.elf
build_wl_app userspace/apps/paint/paint.c             paint
build_wl_app userspace/apps/synth/synth.c             synth
build_wl_app userspace/apps/tetris/tetris.c           tetris
build_wl_app userspace/apps/game2048/game2048.c       game2048
build_wl_app userspace/apps/bubbletd/bubbletd.c       bubbletd
build_wl_app userspace/apps/zombietd/zombietd.c       zombietd
build_wl_app userspace/apps/sheet/sheet.c             sheet
build_wl_app userspace/apps/notes/notes.c             notes
build_wl_app userspace/apps/calendar/calendar.c       calendar
build_wl_app userspace/apps/stopwatch/stopwatch.c     stopwatch
build_wl_app userspace/apps/mines/mines.c             mines
build_wl_app userspace/apps/piano/piano.c             piano
build_wl_app userspace/apps/dashboard/dashboard.c     dashboard
build_wl_app userspace/apps/welcome/welcome.c         welcome
build_wl_app userspace/apps/bench/bench.c             bench

echo "[all] game-framework apps..."
build_game_app userspace/apps/breakout/breakout.c     breakout
build_game_app userspace/apps/pong/pong.c             pong
build_game_app userspace/apps/invaders/invaders.c     invaders
build_game_app userspace/apps/solitaire/solitaire.c   solitaire
build_game_app userspace/apps/chess/chess.c           chess

echo "[all] wl-direct apps (round 7)..."
build_wl_app userspace/apps/aiconsole/aiconsole.c     aiconsole
build_wl_app userspace/apps/screenshot/screenshot.c   screenshot
build_wl_app userspace/apps/stress/stress.c           stress

echo "[all] lib-linked apps (aictl / audio)..."
cc userspace/apps/procmon/procmon.c   /tmp/procmon.o
$LD /tmp/procmon.o /tmp/aictl.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/procmon.elf
cc userspace/apps/soundtest/soundtest.c /tmp/soundtest.o
$LD /tmp/soundtest.o /tmp/audio.o /tmp/wlc.o /tmp/bf.o -o /tmp/soundtest.elf
cc userspace/apps/musicplayer/musicplayer.c /tmp/musicplayer.o
$LD /tmp/musicplayer.o /tmp/audio.o /tmp/wlc.o /tmp/bf.o -o /tmp/musicplayer.elf

echo "[all] image viewer (special flags, best-effort)..."
IV_OK=0
if gcc $CF -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DIMAGE_FREESTANDING -c userspace/apps/imageviewer/imageviewer.c -o /tmp/iv.o \
   && gcc $CF -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DIMAGE_FREESTANDING -c userspace/lib/image/image.c -o /tmp/image.o \
   && cc userspace/libc/stdlib.c /tmp/lstdlib.o && cc userspace/libc/string.c /tmp/lstring.o \
   && cc userspace/libc/math.c /tmp/lmath.o && cc userspace/libc/syscall.c /tmp/lsyscall.o \
   && $LD /tmp/iv.o /tmp/image.o /tmp/wlc.o /tmp/bf.o /tmp/lstdlib.o /tmp/lstring.o /tmp/lmath.o /tmp/lsyscall.o -o /tmp/imageviewer.elf; then
    IV_OK=1; echo "  imageviewer OK"
else
    echo "  imageviewer SKIPPED (build failed)"
fi

echo "[all] IDE (Semantic LEGO Map)..."
IDE_SRCS="ide ide_sys ide_gfx ide_lex ide_ast ide_pcore ide_pdecl ide_pstmt ide_pexpr ide_astprint ide_parse ide_semantic ide_explorer ide_funcs ide_map ide_codeview ide_inspector ide_runtime ide_chrome ide_gen elf_write as_x64 cc_type cc_codegen cc_expr tc_driver ide_build ide_editor ide_term"
IDE_OBJS=""
for s in $IDE_SRCS; do cc userspace/apps/ide/$s.c /tmp/ide_$s.o; IDE_OBJS="$IDE_OBJS /tmp/ide_$s.o"; done
$LD $IDE_OBJS /tmp/wlc.o /tmp/bf.o /tmp/font2.o /tmp/keymap.o -o /tmp/ide.elf

# cc: the on-device C compiler (the self-hosting flagship). It is a thin driver
# that REUSES the IDE's verified toolchain objects (lexer/parser/codegen/
# assembler/ELF writer), so we link the already-compiled /tmp/ide_*.o here rather
# than rebuilding them. Needs -Iuserspace/apps/ide for the IDE headers, and crt0
# for argv. Output programs carry their own _start, so they don't need crt0.
echo "[all] cc (on-device C compiler -- reuses IDE toolchain objects)..."
gcc $CF -Iuserspace/apps/ide -c userspace/apps/cc/cc.c -o /tmp/cc.o
$LD /tmp/crt0.o /tmp/cc.o \
    /tmp/ide_ide_pcore.o /tmp/ide_ide_pdecl.o /tmp/ide_ide_pstmt.o /tmp/ide_ide_pexpr.o \
    /tmp/ide_ide_lex.o /tmp/ide_ide_ast.o \
    /tmp/ide_cc_codegen.o /tmp/ide_cc_expr.o /tmp/ide_cc_type.o \
    /tmp/ide_as_x64.o /tmp/ide_elf_write.o \
    -o /tmp/cc.elf

echo "[all] canary check (all must be 0):"
for e in comp init filemanager calculator clock sysinfo settings sysmon uidemo dateapp applauncher taskman terminal editor snake paint synth tetris game2048 sheet notes calendar stopwatch mines piano dashboard welcome bench breakout pong invaders procmon soundtest solitaire aiconsole screenshot stress musicplayer ide bubbletd zombietd pacman clockapp forktest threadtest matmuljobs aibroker sed awk tar pkg make meminfo argvtest floattest sleeptest prioritytest matbench tensortest cpuburn blk ps kill free uptime find diff cmp tee wcx xargs gzip cc nettest sockettest cpu1offload wget netman browser cryptotest libtest ping nc grep head tail sort uniq cut tr nl du touch basename dirname uname hostname whoami date less hexdump tlsprobe certtool dhcpc apidemo js futextest epolltest sendfiletest perftest batchtest domtest htmltest csstest layouttest webtest browser2 webapitest cube3d ray chess asteroids sudoku photos startmenu controlcenter gametest; do
    n=$(objdump -d /tmp/$e.elf 2>/dev/null | grep -c "fs:0x28" || true)
    echo "  $e=$n"
done

echo "[all] packaging initrd..."
rm -rf /tmp/ird && mkdir -p /tmp/ird/sbin /tmp/ird/bin
# Seed from the existing initrd if it is present and non-empty (preserves any
# static files baked into a prior image). The explicit mkdir above guarantees
# sbin/ and bin/ exist even when iso/boot/initrd.img is the empty bootstrap stub
# (fresh checkout / cleaned tree) — otherwise the first `cp ... /tmp/ird/sbin/`
# below fails with "No such file or directory" and the whole initrd is empty.
( cd /tmp/ird && tar xf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img 2>/dev/null || true )
cp /tmp/comp.elf /tmp/ird/sbin/compositor
cp /tmp/init.elf /tmp/ird/sbin/init
for e in filemanager calculator clock sysinfo settings sysmon uidemo dateapp applauncher taskman terminal editor snake paint synth tetris game2048 sheet notes calendar stopwatch mines piano dashboard welcome bench breakout pong invaders procmon soundtest solitaire aiconsole screenshot stress musicplayer ide bubbletd startmenu controlcenter chess asteroids sudoku photos pacman clockapp zombietd forktest threadtest matmuljobs cube3d ray; do
    cp /tmp/$e.elf /tmp/ird/sbin/$e
done
[ "$IV_OK" = "1" ] && cp /tmp/imageviewer.elf /tmp/ird/sbin/imageviewer
# AI-native layer: broker in /sbin (init spawns sbin/aibroker); standard tools in
# /bin so the shell's external-command fallback can spawn /bin/<cmd>.
mkdir -p /tmp/ird/bin
cp /tmp/aibroker.elf /tmp/ird/sbin/aibroker
cp /tmp/sed.elf      /tmp/ird/bin/sed
cp /tmp/awk.elf      /tmp/ird/bin/awk
cp /tmp/tar.elf      /tmp/ird/bin/tar
cp /tmp/pkg.elf      /tmp/ird/bin/pkg
cp /tmp/make.elf     /tmp/ird/bin/make
cp /tmp/meminfo.elf  /tmp/ird/bin/meminfo
cp /tmp/argvtest.elf /tmp/ird/sbin/argvtest
cp /tmp/floattest.elf /tmp/ird/sbin/floattest
cp /tmp/sleeptest.elf /tmp/ird/sbin/sleeptest
# prioritytest -> /sbin (init spawns it after the boot storm drains). Proves
# priority classes shift CPU share by comparing two children's cpu_ticks.
cp /tmp/prioritytest.elf /tmp/ird/sbin/prioritytest
cp /tmp/gametest.elf /tmp/ird/sbin/gametest
# cpuburn -> /sbin (init spawns sbin/cpuburn under PREEMPT_STRESS). Shipped in
# every initrd but only spawned by a STRESS=1 init, so it is inert otherwise.
cp /tmp/cpuburn.elf /tmp/ird/sbin/cpuburn
cp /tmp/matbench.elf /tmp/ird/sbin/matbench
# tensortest: SSE tensor-kernel correctness self-test (init spawns it at boot).
cp /tmp/tensortest.elf /tmp/ird/sbin/tensortest
# Wave tools: process/disk + file/text utils, gzip, and the on-device C compiler.
for t in blk ps kill free uptime find diff cmp tee wcx xargs gzip cc; do
    cp /tmp/$t.elf /tmp/ird/bin/$t
done
# gunzip is gzip under another argv[0] (basename selects decompress mode).
cp /tmp/gzip.elf /tmp/ird/bin/gunzip
# nettest lives in /sbin (init spawns it like the other on-boot probes).
cp /tmp/nettest.elf /tmp/ird/sbin/nettest
cp /tmp/sockettest.elf /tmp/ird/sbin/sockettest
# cpu1offload -> /sbin (init spawns it at boot; prints PASS on SMP, SKIP on default).
cp /tmp/cpu1offload.elf /tmp/ird/sbin/cpu1offload
# overhaul-syscall verification probes -> /sbin (init spawns them at boot)
for t in futextest epolltest sendfiletest perftest batchtest; do
    cp /tmp/$t.elf /tmp/ird/sbin/$t
done
# browser-wave per-layer selftests + browser2 + webapitest -> /sbin (init spawns at boot).
for t in domtest htmltest csstest layouttest webtest browser2 webapitest; do
    cp /tmp/$t.elf /tmp/ird/sbin/$t
done
# wget is a CLI tool (/bin); netman + browser are GUI apps (/sbin, dock-launchable).
cp /tmp/wget.elf    /tmp/ird/bin/wget
cp /tmp/netman.elf  /tmp/ird/sbin/netman
cp /tmp/browser.elf /tmp/ird/sbin/browser
# net tools + coreutils expansion -> /bin (shell-spawnable).
for t in ping nc grep head tail sort uniq cut tr nl du touch basename dirname uname hostname whoami date less hexdump tlsprobe certtool dhcpc apidemo js; do
    cp /tmp/$t.elf /tmp/ird/bin/$t
done
# KAT self-test harnesses -> /sbin (init spawns them at boot).
cp /tmp/cryptotest.elf /tmp/ird/sbin/cryptotest
cp /tmp/libtest.elf    /tmp/ird/sbin/libtest
# Writable scratch dir so file-creating tools (cc -o, gzip, tee) have a target.
mkdir -p /tmp/ird/tmp
# Sample C project for the IDE to open/parse (the Semantic LEGO Map demo)
mkdir -p /tmp/ird/usr/src/towerdefense
cp userspace/apps/ide/sample/towerdefense/*.c /tmp/ird/usr/src/towerdefense/ 2>/dev/null || true
cp userspace/apps/ide/sample/towerdefense/*.h /tmp/ird/usr/src/towerdefense/ 2>/dev/null || true
# Native-toolchain sample programs (the IDE compiles + runs these on-device)
mkdir -p /tmp/ird/usr/src/native
cp userspace/apps/ide/sample/native/* /tmp/ird/usr/src/native/ 2>/dev/null || true
# Bubble Defense as an IDE project, + a Desktop folder for compiled output
mkdir -p /tmp/ird/usr/src/bubbledefense
cp userspace/apps/bubbletd/bubbletd.c /tmp/ird/usr/src/bubbledefense/ 2>/dev/null || true
# Zombie Bastion game source = the featured IDE example project (opened by default;
# the IDE lists+opens both .c and .asm under it and parses it into the LEGO map).
mkdir -p /tmp/ird/usr/src/zombiebastion
cp userspace/apps/zombietd/zombietd.c /tmp/ird/usr/src/zombiebastion/ 2>/dev/null || true

# IDE "New Project" templates (game / app / service starters) -> /usr/src/templates
mkdir -p /tmp/ird/usr/src/templates/gamestarter /tmp/ird/usr/src/templates/appstarter /tmp/ird/usr/src/templates/servicestarter
cp userspace/apps/ide/sample/gamestarter/*    /tmp/ird/usr/src/templates/gamestarter/    2>/dev/null || true
cp userspace/apps/ide/sample/appstarter/*     /tmp/ird/usr/src/templates/appstarter/     2>/dev/null || true
cp userspace/apps/ide/sample/servicestarter/* /tmp/ird/usr/src/templates/servicestarter/ 2>/dev/null || true
mkdir -p /tmp/ird/Desktop
# Zombie Bastion: a Desktop FOLDER holding the game ELF. The compositor shows
# /Desktop entries as icons; a folder opens the filemanager, where the .elf can
# be launched. (The dock "Zt" icon launches it directly too.)
mkdir -p /tmp/ird/Desktop/ZombieBastion
cp /tmp/zombietd.elf /tmp/ird/Desktop/ZombieBastion/zombietd.elf 2>/dev/null || true
( cd /tmp/ird && tar --format=ustar --owner=0 --group=0 -cf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img . )

echo "[all] installing fresh kernel into ISO tree..."
# Kernel selection. By DEFAULT (FB_WC unset) this installs build/kernel.elf --
# the safe, byte-for-byte default kernel -- so the default ISO is untouched.
# When FB_WC=1 is set we install build/kernel-wc.elf instead (the write-combining
# kernel produced by `FB_WC=1 bash scripts/quick_build.sh`), so the resulting ISO
# is the opt-in WC test image. This mirrors how quick_build.sh writes a separate
# kernel-wc.elf and never lets a WC build clobber the default kernel.elf.
KERNEL_FOR_ISO="build/kernel.elf"
if [ "${FB_WC:-0}" = "1" ]; then
    KERNEL_FOR_ISO="build/kernel-wc.elf"
    if [ ! -f "$KERNEL_FOR_ISO" ]; then
        echo "*** FB_WC=1 but $KERNEL_FOR_ISO missing -- run 'FB_WC=1 bash scripts/quick_build.sh' first ***"
        exit 1
    fi
    echo "*** FB_WC build: installing WRITE-COMBINING kernel $KERNEL_FOR_ISO into ISO ***"
fi
# SMP=1 installs the SMP_FOUNDATION kernel (build/kernel-smp.elf from
# `SMP=1 bash scripts/quick_build.sh`) so the desktop runs on the multi-core kernel
# (needs QEMU -smp 2 / real >=2-core hardware). Mirrors the FB_WC opt-in.
if [ "${SMP:-0}" = "1" ]; then
    KERNEL_FOR_ISO="build/kernel-smp.elf"
    if [ ! -f "$KERNEL_FOR_ISO" ]; then
        echo "*** SMP=1 but $KERNEL_FOR_ISO missing -- run 'SMP=1 bash scripts/quick_build.sh' first ***"
        exit 1
    fi
    echo "*** SMP build: installing SMP_FOUNDATION kernel $KERNEL_FOR_ISO into ISO ***"
fi
cp "$KERNEL_FOR_ISO" iso/boot/kernel.elf

# Write grub.cfg fresh each build. iso/ is gitignored, so build_all.sh is the
# tracked source of truth for the boot config. gfxpayload requests the T410's
# native 1280x800 first, then falls back (1024x768 -> auto) so a framebuffer is
# always set even on a VBE that lacks native. Pairs with boot.asm's multiboot
# header video request (also 1280x800).
echo "[all] writing grub.cfg (native-res request + fallback chain)..."
mkdir -p iso/boot/grub
cat > iso/boot/grub/grub.cfg <<'GRUBCFG'
set timeout=0
set default=0

menuentry 'AutomationOS' {
    set gfxpayload=1280x800x32,1280x800,1024x768x32,1024x768,auto
    multiboot /boot/kernel.elf
    module /boot/initrd.img
}
GRUBCFG

echo "[all] rebuilding ISO..."
grub-mkrescue -o build/automationos.iso iso/ 2>/dev/null
echo "[all] DONE: $(stat -c%s build/automationos.iso) byte ISO, $(ls /tmp/ird/sbin | wc -l) sbin entries"
