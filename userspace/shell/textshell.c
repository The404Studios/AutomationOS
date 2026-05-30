// textshell.c - AutomationOS Interactive Shell v0.3
// Built by Elijah Isaiah Roberts (fourzerofour)
// Freestanding C, no libc - raw syscalls only

#include "../libc/syscall.h"
#include "../libc/string.h"

// ─── Utility ────────────────────────────────────────────

static void print(const char* s) {
    write(1, s, strlen(s));
}

static void println(const char* s) {
    print(s);
    write(1, "\n", 1);
}

static void print_int(int n) {
    if (n < 0) { write(1, "-", 1); n = -n; }
    if (n == 0) { write(1, "0", 1); return; }
    char buf[16];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) { i--; write(1, &buf[i], 1); }
}

static void print_hex(unsigned long n) {
    print("0x");
    char hex[] = "0123456789abcdef";
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int digit = (n >> i) & 0xF;
        if (digit || started || i == 0) {
            char c = hex[digit];
            write(1, &c, 1);
            started = 1;
        }
    }
}

static void repeat_char(char c, int count) {
    for (int i = 0; i < count; i++)
        write(1, &c, 1);
}

static int str_to_int(const char* s) {
    int n = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return neg ? -n : n;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// ─── Command History ────────────────────────────────────

#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][128];
static int history_count = 0;

static void history_add(const char* cmd) {
    if (cmd[0] == '\0') return;
    if (history_count > 0 && strcmp(history[history_count - 1], cmd) == 0) return;
    if (history_count < HISTORY_SIZE) {
        strcpy(history[history_count++], cmd);
    } else {
        for (int i = 0; i < HISTORY_SIZE - 1; i++)
            strcpy(history[i], history[i + 1]);
        strcpy(history[HISTORY_SIZE - 1], cmd);
    }
}

// ─── Alias System ───────────────────────────────────────

#define MAX_ALIASES 16
static char alias_names[MAX_ALIASES][32];
static char alias_values[MAX_ALIASES][96];
static int alias_count = 0;

static const char* alias_lookup(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(alias_names[i], name) == 0)
            return alias_values[i];
    }
    return (void*)0;
}

// ─── Environment Variables ──────────────────────────────

#define MAX_VARS 16
static char var_names[MAX_VARS][32];
static char var_values[MAX_VARS][96];
static int var_count = 0;

static void var_set(const char* name, const char* value) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(var_names[i], name) == 0) {
            strncpy(var_values[i], value, 95);
            var_values[i][95] = '\0';
            return;
        }
    }
    if (var_count < MAX_VARS) {
        strncpy(var_names[var_count], name, 31);
        var_names[var_count][31] = '\0';
        strncpy(var_values[var_count], value, 95);
        var_values[var_count][95] = '\0';
        var_count++;
    }
}

static const char* var_get(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(var_names[i], name) == 0)
            return var_values[i];
    }
    return (void*)0;
}

// ─── Commands: System ───────────────────────────────────

static void cmd_help(void) {
    println("");
    println("  System");
    println("    help              show this message");
    println("    about             about AutomationOS");
    println("    uname             system information");
    println("    clear / cls       clear screen");
    println("    uptime            system status");
    println("    whoami            current user");
    println("    hostname          machine name");
    println("    pid               show process ID");
    println("    version           kernel version");
    println("    desktop / startx  launch graphical desktop");
    println("");
    println("  Process");
    println("    spawn <path>      launch program from initrd");
    println("    ls                list files in initrd");
    println("");
    println("  Text & I/O");
    println("    echo <text>       print text");
    println("    cat <var>         print variable value");
    println("    set <n>=<v>       set variable");
    println("    env               list variables");
    println("");
    println("  Tools");
    println("    calc <expr>       math (e.g. calc 42 + 7)");
    println("    hex <num>         decimal to hex");
    println("    bin <num>         decimal to binary");
    println("    base <b> <num>    convert to base (2-16)");
    println("    len <text>        string length");
    println("    reverse <text>    reverse text");
    println("    upper <text>      to uppercase");
    println("    lower <text>      to lowercase");
    println("    count <n>         count 1 to n");
    println("    seq <a> <b>       sequence from a to b");
    println("    repeat <n> <c>    repeat character");
    println("");
    println("  Shell");
    println("    history           command history");
    println("    alias <n>=<cmd>   create alias");
    println("    aliases           list aliases");
    println("    !! / r            repeat last command");
    println("");
    println("  Fun");
    println("    hello             greeting");
    println("    cowsay <text>     cow says it");
    println("    fortune           random wisdom");
    println("    ascii             logo art");
    println("    banner <text>     big banner");
    println("    matrix            matrix rain");
    println("    dice              roll a d6");
    println("    flip              flip a coin");
    println("    8ball <question>  magic 8-ball");
    println("");
}

static void cmd_about(void) {
    println("");
    println("  AutomationOS v0.1.0");
    println("  ===================");
    println("");
    println("  Built by: Elijah Isaiah Roberts (fourzerofour)");
    println("");
    println("  A complete operating system built entirely from");
    println("  scratch in a single session. No libraries, no");
    println("  borrowed code, no shortcuts. Every byte of this");
    println("  kernel was reasoned into existence from first");
    println("  principles.");
    println("");
    println("  Architecture:");
    println("    x86_64 long mode      4-level paging");
    println("    Per-process CR3        Ring 0/3 isolation");
    println("    SYSCALL/SYSRET        PS/2 keyboard IRQ");
    println("    ELF64 loader           TAR initrd");
    println("    Cooperative scheduler  VGA text output");
    println("");
    println("  \"Architecture by intuition.\"");
    println("");
}

static void cmd_version(void) {
    println("AutomationOS v0.1.0 (fourzerofour)");
    println("Kernel: monolithic, freestanding, x86_64");
    println("Shell:  v0.3 freestanding C");
}

static void cmd_uname(void) {
    println("AutomationOS v0.1.0 x86_64 fourzerofour");
}

static void cmd_clear(void) {
    for (int i = 0; i < 25; i++) write(1, "\n", 1);
}

static void cmd_whoami(void) { println("root"); }
static void cmd_hostname(void) { println("automationos"); }
static void cmd_uptime(void) {
    println("Status: All systems operational");
    println("Mode:   Cooperative scheduling (SYS_YIELD)");
    println("Procs:  init (PID 1), shell (PID 2)");
}

static void cmd_pid(void) {
    print("PID: ");
    print_int(getpid());
    println("");
}

// ─── Commands: Text & Variables ─────────────────────────

static void cmd_set(const char* args) {
    char name[32];
    int i = 0;
    while (args[i] && args[i] != '=' && i < 31) {
        name[i] = args[i];
        i++;
    }
    name[i] = '\0';
    if (args[i] != '=') { println("Usage: set name=value"); return; }
    var_set(name, args + i + 1);
    print(name);
    print(" = ");
    println(args + i + 1);
}

static void cmd_cat_var(const char* name) {
    const char* val = var_get(name);
    if (val) println(val);
    else { print("Variable not found: "); println(name); }
}

static void cmd_env(void) {
    if (var_count == 0) { println("No variables set. Use: set name=value"); return; }
    for (int i = 0; i < var_count; i++) {
        print("  ");
        print(var_names[i]);
        print(" = ");
        println(var_values[i]);
    }
}

static void cmd_ls(void) {
    println("  /sbin/init          kernel init process");
    println("  /sbin/shell         this shell (v0.3)");
    println("  /sbin/compositor    (placeholder)");
    println("  /sbin/wm            (placeholder)");
    println("  /sbin/terminal      echo terminal");
    println("  /bin/files.bin      file manager stub");
    println("  /bin/settings.bin   settings stub");
    println("  /bin/terminal.bin   terminal stub");
    println("  /bin/taskmanager.bin task manager stub");
    println("  /etc/fstab          filesystem table");
    println("  /etc/inittab        init config");
}

// ─── Commands: Tools ────────────────────────────────────

static void cmd_calc(const char* expr) {
    int a = 0, b = 0;
    char op = 0;
    const char* p = expr;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
    if (neg) a = -a;
    while (*p == ' ') p++;
    op = *p; if (op) p++;
    while (*p == ' ') p++;
    neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
    if (neg) b = -b;

    if (!op) { println("Usage: calc <a> <op> <b>  (+ - * / %)"); return; }
    int result = 0;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/': if (b == 0) { println("Division by zero"); return; } result = a / b; break;
        case '%': if (b == 0) { println("Division by zero"); return; } result = a % b; break;
        default: print("Unknown op: "); write(1, &op, 1); println(""); return;
    }
    print_int(a); print(" "); write(1, &op, 1); print(" "); print_int(b);
    print(" = "); print_int(result); println("");
}

static void cmd_hex(const char* s) {
    int n = str_to_int(s);
    print_int(n); print(" = "); print_hex((unsigned long)n); println("");
}

static void cmd_bin(const char* s) {
    int n = str_to_int(s);
    print_int(n); print(" = 0b");
    int started = 0;
    for (int i = 31; i >= 0; i--) {
        int bit = (n >> i) & 1;
        if (bit || started || i == 0) {
            char c = '0' + bit;
            write(1, &c, 1);
            started = 1;
        }
    }
    println("");
}

static void cmd_base(const char* args) {
    int base = 0;
    const char* p = args;
    while (*p >= '0' && *p <= '9') { base = base * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    int n = str_to_int(p);
    if (base < 2 || base > 16) { println("Base must be 2-16"); return; }

    print_int(n); print(" in base "); print_int(base); print(" = ");
    char digits[] = "0123456789abcdef";
    char buf[64];
    int i = 0;
    unsigned int un = (unsigned int)n;
    if (un == 0) { buf[i++] = '0'; }
    else { while (un > 0) { buf[i++] = digits[un % base]; un /= base; } }
    while (i > 0) { i--; write(1, &buf[i], 1); }
    println("");
}

static void cmd_len(const char* s) { print_int(strlen(s)); println(" characters"); }

static void cmd_reverse(const char* s) {
    int l = strlen(s);
    for (int i = l - 1; i >= 0; i--) write(1, &s[i], 1);
    println("");
}

static void cmd_upper(const char* s) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        write(1, &c, 1);
    }
    println("");
}

static void cmd_lower(const char* s) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        write(1, &c, 1);
    }
    println("");
}

static void cmd_count(const char* s) {
    int n = str_to_int(s);
    if (n <= 0 || n > 200) { println("Count 1-200"); return; }
    for (int i = 1; i <= n; i++) {
        print_int(i);
        if (i < n) print(" ");
    }
    println("");
}

static void cmd_seq(const char* args) {
    int a = 0, b = 0;
    const char* p = args;
    a = str_to_int(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    b = str_to_int(p);
    if (a == b) { print_int(a); println(""); return; }
    int step = (a < b) ? 1 : -1;
    for (int i = a; ; i += step) {
        print_int(i);
        if (i == b) break;
        print(" ");
    }
    println("");
}

static void cmd_repeat(const char* args) {
    int n = 0;
    const char* p = args;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    char c = *p ? *p : '*';
    if (n <= 0 || n > 200) n = 20;
    repeat_char(c, n);
    println("");
}

// ─── Commands: Shell ────────────────────────────────────

static void cmd_history_show(void) {
    if (history_count == 0) { println("No history yet"); return; }
    for (int i = 0; i < history_count; i++) {
        print("  ");
        print_int(i + 1);
        print("  ");
        println(history[i]);
    }
}

static void cmd_alias(const char* args) {
    char name[32];
    int i = 0;
    while (args[i] && args[i] != '=' && i < 31) { name[i] = args[i]; i++; }
    name[i] = '\0';
    if (args[i] != '=') { println("Usage: alias name=command"); return; }
    if (alias_count >= MAX_ALIASES) { println("Alias table full"); return; }
    strncpy(alias_names[alias_count], name, 31);
    alias_names[alias_count][31] = '\0';
    strncpy(alias_values[alias_count], args + i + 1, 95);
    alias_values[alias_count][95] = '\0';
    alias_count++;
    print("Alias: "); print(name); print(" -> "); println(args + i + 1);
}

static void cmd_aliases(void) {
    if (alias_count == 0) { println("No aliases. Use: alias name=command"); return; }
    for (int i = 0; i < alias_count; i++) {
        print("  "); print(alias_names[i]); print(" = "); println(alias_values[i]);
    }
}

// ─── Commands: Fun ──────────────────────────────────────

static unsigned long rand_state = 42;
static int rand_int(int max) {
    rand_state = rand_state * 1103515245 + 12345;
    return (int)((rand_state >> 16) % max);
}

static void cmd_hello(void) {
    const char* g[] = {
        "Hello! Welcome to AutomationOS!",
        "Hey there! The kernel is happy to see you.",
        "Greetings from ring 3!",
        "Welcome back! Systems ready.",
        "All systems nominal. Ready for commands.",
    };
    println(g[rand_int(5)]);
    rand_state += 7;
}

static void cmd_cowsay(const char* text) {
    int len = strlen(text);
    if (len == 0) { text = "Moo!"; len = 4; }
    if (len > 50) len = 50;
    print(" "); repeat_char('_', len + 2); println("");
    print("< "); write(1, text, len); println(" >");
    print(" "); repeat_char('-', len + 2); println("");
    println("        \\   ^__^");
    println("         \\  (oo)\\_______");
    println("            (__)\\       )\\/\\");
    println("                ||----w |");
    println("                ||     ||");
}

static void cmd_fortune(void) {
    const char* f[] = {
        "The kernel is the foundation. Everything else is userspace.",
        "In ring 0, no one can hear you scream.",
        "A page fault a day keeps the developer awake.",
        "The best OS is the one you build yourself.",
        "Every expert was once a beginner who refused to quit.",
        "Simplicity is the ultimate sophistication.",
        "Today's accomplishment was yesterday's impossibility.",
        "You built an OS from scratch. What's stopping you now?",
        "Architecture by intuition. -- fourzerofour, 2026",
        "First, solve the problem. Then, write the code.",
        "One session. From nothing to interactive shell.",
        "The QEMU window doesn't care about credentials. It runs or it doesn't.",
    };
    println(f[rand_int(12)]);
    rand_state += 13;
}

static void cmd_ascii(void) {
    println("");
    println("     _         _        ___  ____  ");
    println("    / \\  _   _| |_ ___ / _ \\/ ___| ");
    println("   / _ \\| | | | __/ _ \\ | | \\___ \\ ");
    println("  / ___ \\ |_| | || (_) | |_| |___) |");
    println(" /_/   \\_\\__,_|\\__\\___/ \\___/|____/ ");
    println("");
}

static void cmd_banner(const char* text) {
    int len = strlen(text);
    if (len == 0) return;
    println("");
    print("  "); repeat_char('#', len + 6); println("");
    print("  #  ");
    for (int i = 0; i < len; i++) {
        char c = text[i]; if (c >= 'a' && c <= 'z') c -= 32;
        write(1, &c, 1);
    }
    println("  #");
    print("  "); repeat_char('#', len + 6); println("");
    println("");
}

static void cmd_matrix(void) {
    println("Wake up, Neo...");
    println("The Matrix has you...");
    println("Follow the white rabbit.");
    println("");
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 40; col++) {
            char c = '0' + rand_int(10);
            if (rand_int(3) == 0) c = ' ';
            write(1, &c, 1);
        }
        println("");
        rand_state += 13;
    }
}

static void cmd_dice(void) {
    int roll = rand_int(6) + 1;
    print("Rolling... ");
    print_int(roll);
    println("!");
    rand_state += 3;
}

static void cmd_flip(void) {
    println(rand_int(2) ? "Heads!" : "Tails!");
    rand_state += 5;
}

static void cmd_8ball(const char* q) {
    if (strlen(q) == 0) { println("Ask a question!"); return; }
    const char* answers[] = {
        "It is certain.",
        "Without a doubt.",
        "Most likely.",
        "Yes, definitely.",
        "Ask again later.",
        "Cannot predict now.",
        "Don't count on it.",
        "My reply is no.",
        "Outlook not so good.",
        "Signs point to yes.",
    };
    print("Q: "); println(q);
    print("8-ball: "); println(answers[rand_int(10)]);
    rand_state += 11;
}

// ─── Command Dispatch ───────────────────────────────────

static void handle_command(const char* cmd);

static void dispatch(const char* cmd) {
    while (*cmd == ' ') cmd++;
    if (cmd[0] == '\0') return;

    // Check aliases first
    char first_word[32];
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < 31) { first_word[i] = cmd[i]; i++; }
    first_word[i] = '\0';
    const char* alias_val = alias_lookup(first_word);
    if (alias_val) {
        char expanded[128];
        unsigned long used = 0;

        while (alias_val[used] && used < sizeof(expanded) - 1) {
            expanded[used] = alias_val[used];
            used++;
        }

        if (alias_val[used] != '\0') {
            println("alias expansion too long");
            return;
        }

        if (cmd[i] == ' ') {
            const char* suffix = cmd + i;
            while (*suffix && used < sizeof(expanded) - 1) {
                expanded[used++] = *suffix++;
            }
            if (*suffix) {
                println("alias expansion too long");
                return;
            }
        }

        expanded[used] = '\0';
        handle_command(expanded);
        return;
    }

    // System
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)           cmd_help();
    else if (strcmp(cmd, "about") == 0)                               cmd_about();
    else if (strcmp(cmd, "version") == 0)                             cmd_version();
    else if (strcmp(cmd, "uname") == 0 || strcmp(cmd, "uname -a") == 0) cmd_uname();
    else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0)   cmd_clear();
    else if (strcmp(cmd, "uptime") == 0)                              cmd_uptime();
    else if (strcmp(cmd, "whoami") == 0)                              cmd_whoami();
    else if (strcmp(cmd, "hostname") == 0)                            cmd_hostname();
    else if (strcmp(cmd, "pid") == 0)                                 cmd_pid();
    else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0)      cmd_ls();

    // Text & Variables
    else if (strncmp(cmd, "echo ", 5) == 0)                          println(cmd + 5);
    else if (strncmp(cmd, "set ", 4) == 0)                           cmd_set(cmd + 4);
    else if (strncmp(cmd, "cat ", 4) == 0)                           cmd_cat_var(cmd + 4);
    else if (strcmp(cmd, "env") == 0)                                 cmd_env();

    // Tools
    else if (strncmp(cmd, "calc ", 5) == 0)                          cmd_calc(cmd + 5);
    else if (strncmp(cmd, "hex ", 4) == 0)                           cmd_hex(cmd + 4);
    else if (strncmp(cmd, "bin ", 4) == 0)                           cmd_bin(cmd + 4);
    else if (strncmp(cmd, "base ", 5) == 0)                          cmd_base(cmd + 5);
    else if (strncmp(cmd, "len ", 4) == 0)                           cmd_len(cmd + 4);
    else if (strncmp(cmd, "reverse ", 8) == 0)                       cmd_reverse(cmd + 8);
    else if (strncmp(cmd, "upper ", 6) == 0)                         cmd_upper(cmd + 6);
    else if (strncmp(cmd, "lower ", 6) == 0)                         cmd_lower(cmd + 6);
    else if (strncmp(cmd, "count ", 6) == 0)                         cmd_count(cmd + 6);
    else if (strncmp(cmd, "seq ", 4) == 0)                           cmd_seq(cmd + 4);
    else if (strncmp(cmd, "repeat ", 7) == 0)                        cmd_repeat(cmd + 7);

    // Shell
    else if (strcmp(cmd, "history") == 0)                             cmd_history_show();
    else if (strncmp(cmd, "alias ", 6) == 0)                         cmd_alias(cmd + 6);
    else if (strcmp(cmd, "aliases") == 0)                             cmd_aliases();

    // Fun
    else if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "hi") == 0)    cmd_hello();
    else if (strncmp(cmd, "cowsay ", 7) == 0)                        cmd_cowsay(cmd + 7);
    else if (strcmp(cmd, "cowsay") == 0)                              cmd_cowsay("");
    else if (strcmp(cmd, "fortune") == 0)                             cmd_fortune();
    else if (strcmp(cmd, "ascii") == 0)                               cmd_ascii();
    else if (strncmp(cmd, "banner ", 7) == 0)                        cmd_banner(cmd + 7);
    else if (strcmp(cmd, "matrix") == 0)                              cmd_matrix();
    else if (strcmp(cmd, "dice") == 0 || strcmp(cmd, "roll") == 0)   cmd_dice();
    else if (strcmp(cmd, "flip") == 0 || strcmp(cmd, "coin") == 0)   cmd_flip();
    else if (strncmp(cmd, "8ball ", 6) == 0)                         cmd_8ball(cmd + 6);

    // Process
    else if (strncmp(cmd, "spawn ", 6) == 0) {
        int pid = spawn(cmd + 6);
        if (pid > 0) { print("PID "); print_int(pid); println(" started"); }
        else println("Failed to spawn");
    }

    // Desktop
    else if (strcmp(cmd, "desktop") == 0 || strcmp(cmd, "startx") == 0) {
        println("");
        println("  Starting AutomationOS Desktop...");
        int pid = spawn("sbin/desktop");
        if (pid > 0) {
            print("  Desktop launched (PID ");
            print_int(pid);
            println(")");
        } else {
            println("  Failed to start desktop");
        }
        println("");
    }

    // Unknown
    else { print("Unknown: "); println(cmd); println("Type 'help' for commands"); }
}

static void handle_command(const char* cmd) {
    while (*cmd == ' ') cmd++;
    if (cmd[0] == '\0') return;
    history_add(cmd);
    dispatch(cmd);
}

// ─── Main ───────────────────────────────────────────────

void main(void) {
    // Initialize default variables
    var_set("USER", "root");
    var_set("HOST", "automationos");
    var_set("SHELL", "/sbin/shell");
    var_set("AUTHOR", "Elijah Isaiah Roberts");
    var_set("HANDLE", "fourzerofour");

    println("");
    println("  ==========================================");
    println("    AutomationOS Shell v0.3");
    println("    Type 'help' or 'about'");
    println("  ==========================================");
    println("");
    print("aos> ");

    char line[128];
    int pos = 0;

    while (1) {
        int c = read_key();
        if (c == 0) { yield(); continue; }

        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            line[pos] = '\0';

            // Handle !! (repeat last)
            if (strcmp(line, "!!") == 0 || strcmp(line, "r") == 0) {
                if (history_count > 0) {
                    print("=> ");
                    println(history[history_count - 1]);
                    handle_command(history[history_count - 1]);
                } else {
                    println("No previous command");
                }
            } else {
                handle_command(line);
            }

            pos = 0;
            print("aos> ");
        } else if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                write(1, "\b", 1);
            }
        } else if (c >= 32 && pos < 126) {
            line[pos++] = (char)c;
            char ch = (char)c;
            write(1, &ch, 1);
        }
    }
}
