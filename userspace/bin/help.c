/*
 * userspace/bin/help.c - Command help system
 *
 * Provides detailed help for shell commands and utilities.
 */

typedef unsigned long size_t;

#define SYS_EXIT    0
#define SYS_WRITE   3

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void print(const char* msg) {
    syscall(SYS_WRITE, 1, (long)msg, strlen(msg));
}

static void show_help_ls(void) {
    print("\033[1mls\033[0m - List directory contents\n\n");
    print("Usage: ls [OPTIONS] [PATH]\n\n");
    print("Options:\n");
    print("  -l    Long format (permissions, size, date)\n");
    print("  -a    Show hidden files (starting with .)\n");
    print("  -h    Human-readable file sizes\n");
    print("  -R    Recursive listing\n\n");
    print("Examples:\n");
    print("  ls              List current directory\n");
    print("  ls -la /home    List /home with all files in long format\n");
    print("  ls -lh          Long format with human-readable sizes\n");
}

static void show_help_cat(void) {
    print("\033[1mcat\033[0m - Concatenate and display files\n\n");
    print("Usage: cat [FILE]...\n\n");
    print("Display contents of FILE(s) to standard output.\n\n");
    print("Examples:\n");
    print("  cat file.txt           Display file.txt\n");
    print("  cat file1 file2        Display multiple files\n");
}

static void show_help_grep(void) {
    print("\033[1mgrep\033[0m - Search for patterns in files\n\n");
    print("Usage: grep [OPTIONS] PATTERN [FILE]...\n\n");
    print("Options:\n");
    print("  -i    Case-insensitive search\n");
    print("  -n    Show line numbers\n");
    print("  -r    Recursive search in directories\n");
    print("  -v    Invert match (show non-matching lines)\n\n");
    print("Examples:\n");
    print("  grep error log.txt     Search for 'error' in log.txt\n");
    print("  grep -in main *.c      Case-insensitive search for 'main'\n");
}

static void show_help_top(void) {
    print("\033[1mtop\033[0m - Display running processes\n\n");
    print("Usage: top\n\n");
    print("Real-time system monitor showing:\n");
    print("  - CPU usage per process\n");
    print("  - Memory consumption\n");
    print("  - Process state (RUNNING, SLEEPING, etc.)\n\n");
    print("Updates automatically. Press Ctrl+C to exit.\n");
}

static void show_help_ifconfig(void) {
    print("\033[1mifconfig\033[0m - Network interface configuration\n\n");
    print("Usage: ifconfig [INTERFACE] [OPTIONS]\n\n");
    print("Display or configure network interfaces.\n\n");
    print("Examples:\n");
    print("  ifconfig              Show all interfaces\n");
    print("  ifconfig eth0         Show eth0 interface\n");
    print("  ifconfig eth0 up      Bring eth0 up\n");
}

static void show_general_help(void) {
    print("\n");
    print("\033[1;36m");
    print("╔═══════════════════════════════════════════════════════════╗\n");
    print("║              AutomationOS Help System                     ║\n");
    print("╚═══════════════════════════════════════════════════════════╝\n");
    print("\033[0m\n");

    print("Available Commands:\n");
    print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");

    print("\033[1mFile Operations:\033[0m\n");
    print("  ls        List directory contents\n");
    print("  cat       Display file contents\n");
    print("  cp        Copy files\n");
    print("  mv        Move/rename files\n");
    print("  rm        Remove files\n");
    print("  mkdir     Create directories\n");
    print("  touch     Create empty file\n\n");

    print("\033[1mSystem Information:\033[0m\n");
    print("  top       Process monitor\n");
    print("  ps        List processes\n");
    print("  df        Disk space usage\n");
    print("  free      Memory usage\n");
    print("  uptime    System uptime\n\n");

    print("\033[1mNetworking:\033[0m\n");
    print("  ifconfig  Network interfaces\n");
    print("  ping      Test connectivity\n");
    print("  netstat   Network statistics\n\n");

    print("\033[1mText Processing:\033[0m\n");
    print("  grep      Search in files\n");
    print("  sed       Stream editor\n");
    print("  awk       Pattern processing\n\n");

    print("For detailed help on a command, use:\n");
    print("  \033[1mhelp <command>\033[0m\n\n");

    print("Example: help ls\n\n");
}

void _start(int argc, char** argv) {
    if (argc < 2) {
        show_general_help();
    } else {
        const char* cmd = argv[1];

        if (strcmp(cmd, "ls") == 0) {
            show_help_ls();
        } else if (strcmp(cmd, "cat") == 0) {
            show_help_cat();
        } else if (strcmp(cmd, "grep") == 0) {
            show_help_grep();
        } else if (strcmp(cmd, "top") == 0) {
            show_help_top();
        } else if (strcmp(cmd, "ifconfig") == 0) {
            show_help_ifconfig();
        } else {
            print("No help available for: ");
            print(cmd);
            print("\n\nUse 'help' to see all available commands.\n");
        }
    }

    syscall(SYS_EXIT, 0, 0, 0);
}
