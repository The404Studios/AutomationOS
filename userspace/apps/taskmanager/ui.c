// userspace/apps/taskmanager/ui.c - UI rendering

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

// ANSI color codes
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BG_BLACK "\033[40m"
#define COLOR_BG_WHITE "\033[47m"

// Clear screen
void clear_screen(void) {
    printf("\033[2J\033[H");
}

// Move cursor
void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

// Render main UI
void render_ui(const ui_state_t* ui, const process_info_t* procs, int proc_count,
               const system_stats_t* stats, const perf_history_t* history) {
    clear_screen();

    // Render header with tabs
    render_header(ui->current_tab);

    // Render current tab content
    switch (ui->current_tab) {
        case TAB_PROCESSES:
            render_processes_tab(ui, procs, proc_count);
            break;
        case TAB_PERFORMANCE:
            render_performance_tab(stats, history);
            break;
        case TAB_SERVICES:
            render_services_tab();
            break;
        default:
            break;
    }

    // Render footer with system summary
    render_footer(stats);
}

// Render header with tabs
void render_header(ui_tab_t current_tab) {
    printf(COLOR_BOLD COLOR_BG_WHITE);
    printf("  ");

    // Processes tab
    if (current_tab == TAB_PROCESSES) {
        printf(COLOR_GREEN "[Processes]" COLOR_RESET COLOR_BG_WHITE);
    } else {
        printf(" Processes ");
    }
    printf("  ");

    // Performance tab
    if (current_tab == TAB_PERFORMANCE) {
        printf(COLOR_GREEN "[Performance]" COLOR_RESET COLOR_BG_WHITE);
    } else {
        printf(" Performance ");
    }
    printf("  ");

    // Services tab
    if (current_tab == TAB_SERVICES) {
        printf(COLOR_GREEN "[Services]" COLOR_RESET COLOR_BG_WHITE);
    } else {
        printf(" Services ");
    }

    printf(COLOR_RESET);
    printf("\n");
    printf("════════════════════════════════════════════════════════════════════════════════\n");
}

// Render processes tab
void render_processes_tab(const ui_state_t* ui, const process_info_t* procs, int count) {
    printf("\n");

    // Column headers
    printf(COLOR_BOLD);
    printf("%-6s %-20s %-8s %-10s %-8s %-10s %-10s %s\n",
           "PID", "Name", "State", "User", "CPU%%", "Memory", "Disk I/O", "Net I/O");
    printf(COLOR_RESET);
    printf("────────────────────────────────────────────────────────────────────────────────\n");

    // Process rows (show up to 15 processes)
    int visible_count = (count > 15) ? 15 : count;
    char buf1[32], buf2[32], buf3[32];

    for (int i = ui->scroll_offset; i < ui->scroll_offset + visible_count && i < count; i++) {
        const process_info_t* p = &procs[i];

        // Highlight selected process
        if (i == (int)ui->selected_process) {
            printf(COLOR_BG_WHITE COLOR_BLUE);
        }

        // PID
        printf("%-6d ", p->pid);

        // Name (truncate if too long)
        char name_buf[21];
        if (strlen(p->name) > 20) {
            memcpy(name_buf, p->name, 17);
            strcpy(name_buf + 17, "...");
        } else {
            strcpy(name_buf, p->name);
        }
        printf("%-20s ", name_buf);

        // State with color
        const char* state_str = state_to_string(p->state);
        if (p->state == PROC_RUNNING) {
            printf(COLOR_GREEN "%-8s" COLOR_RESET " ", state_str);
        } else if (p->state == PROC_BLOCKED) {
            printf(COLOR_YELLOW "%-8s" COLOR_RESET " ", state_str);
        } else {
            printf("%-8s ", state_str);
        }

        // User
        printf("%-10s ", p->username);

        // CPU%
        if (p->cpu_percent > 50) {
            printf(COLOR_RED "%6llu%%" COLOR_RESET " ", p->cpu_percent);
        } else if (p->cpu_percent > 25) {
            printf(COLOR_YELLOW "%6llu%%" COLOR_RESET " ", p->cpu_percent);
        } else {
            printf("%6llu%% ", p->cpu_percent);
        }

        // Memory
        printf("%-10s ", format_bytes(p->memory_rss, buf1, sizeof(buf1)));

        // Disk I/O
        uint64_t disk_total = p->disk_read + p->disk_write;
        printf("%-10s ", format_rate(disk_total, buf2, sizeof(buf2)));

        // Network I/O
        uint64_t net_total = p->net_recv + p->net_send;
        printf("%s", format_rate(net_total, buf3, sizeof(buf3)));

        printf(COLOR_RESET "\n");
    }

    // Show sorting mode and filter
    printf("\n");
    printf(COLOR_CYAN "Sort: " COLOR_RESET);
    const char* sort_names[] = {"PID", "Name", "CPU", "Memory", "Disk", "Network"};
    printf("%s %s", sort_names[ui->sort_mode], ui->sort_ascending ? "▲" : "▼");

    if (ui->search_filter[0] != '\0') {
        printf(COLOR_CYAN "  Filter: " COLOR_RESET "%s", ui->search_filter);
    }
    printf("\n");

    // Show controls
    printf("\n");
    printf(COLOR_BOLD "Controls:" COLOR_RESET "\n");
    printf("  ↑/↓: Select  Enter: Details  K: Kill  S: Suspend  R: Resume  P: Priority\n");
    printf("  1-6: Sort by column  /: Search  Q: Quit\n");
}

// Render performance tab
void render_performance_tab(const system_stats_t* stats, const perf_history_t* history) {
    printf("\n");

    // CPU usage
    printf(COLOR_BOLD "CPU Usage" COLOR_RESET " (%d cores)\n", stats->cpu_count);
    printf("────────────────────────────────────────────────────────────────────────────────\n");

    // Per-core usage
    for (int i = 0; i < stats->cpu_count; i++) {
        printf("  Core %d: [", i);
        int bars = stats->cpu_usage[i] / 5;  // Scale to 20 chars
        for (int j = 0; j < 20; j++) {
            if (j < bars) {
                if (stats->cpu_usage[i] > 80) {
                    printf(COLOR_RED "█" COLOR_RESET);
                } else if (stats->cpu_usage[i] > 50) {
                    printf(COLOR_YELLOW "█" COLOR_RESET);
                } else {
                    printf(COLOR_GREEN "█" COLOR_RESET);
                }
            } else {
                printf("░");
            }
        }
        printf("] %2llu%%\n", stats->cpu_usage[i]);
    }

    // Total CPU
    printf("  Total:  [");
    int total_bars = stats->cpu_total_usage / 5;
    for (int j = 0; j < 20; j++) {
        if (j < total_bars) {
            printf(COLOR_CYAN "█" COLOR_RESET);
        } else {
            printf("░");
        }
    }
    printf("] %2llu%%\n", stats->cpu_total_usage);

    printf("\n");

    // Memory usage
    printf(COLOR_BOLD "Memory Usage" COLOR_RESET "\n");
    printf("────────────────────────────────────────────────────────────────────────────────\n");

    uint64_t mem_percent = stats->memory_total ? (stats->memory_used * 100) / stats->memory_total : 0;
    char buf_used[32], buf_total[32];
    printf("  Used:   %s / %s (%llu%%)\n",
           format_bytes(stats->memory_used, buf_used, sizeof(buf_used)),
           format_bytes(stats->memory_total, buf_total, sizeof(buf_total)),
           mem_percent);

    printf("  [");
    int mem_bars = mem_percent / 5;
    for (int j = 0; j < 20; j++) {
        if (j < mem_bars) {
            if (mem_percent > 80) {
                printf(COLOR_RED "█" COLOR_RESET);
            } else if (mem_percent > 60) {
                printf(COLOR_YELLOW "█" COLOR_RESET);
            } else {
                printf(COLOR_GREEN "█" COLOR_RESET);
            }
        } else {
            printf("░");
        }
    }
    printf("]\n");

    char buf_cached[32], buf_buffers[32];
    printf("  Cached: %s    Buffers: %s\n",
           format_bytes(stats->memory_cached, buf_cached, sizeof(buf_cached)),
           format_bytes(stats->memory_buffers, buf_buffers, sizeof(buf_buffers)));

    printf("\n");

    // Disk I/O
    printf(COLOR_BOLD "Disk I/O" COLOR_RESET "\n");
    printf("────────────────────────────────────────────────────────────────────────────────\n");
    char buf_dread[32], buf_dwrite[32];
    printf("  Read:  %s    Write: %s\n",
           format_rate(stats->disk_read_rate, buf_dread, sizeof(buf_dread)),
           format_rate(stats->disk_write_rate, buf_dwrite, sizeof(buf_dwrite)));

    printf("\n");

    // Network I/O
    printf(COLOR_BOLD "Network I/O" COLOR_RESET "\n");
    printf("────────────────────────────────────────────────────────────────────────────────\n");
    char buf_nrecv[32], buf_nsend[32];
    printf("  Recv:  %s    Send: %s\n",
           format_rate(stats->net_recv_rate, buf_nrecv, sizeof(buf_nrecv)),
           format_rate(stats->net_send_rate, buf_nsend, sizeof(buf_nsend)));

    printf("\n");

    // CPU history graph
    printf(COLOR_BOLD "CPU History (60s)" COLOR_RESET "\n");
    render_graph("CPU", history->cpu_history, HISTORY_SAMPLES, 100);
}

// Render services tab
void render_services_tab(void) {
    printf("\n");
    printf(COLOR_BOLD "System Services" COLOR_RESET "\n");
    printf("────────────────────────────────────────────────────────────────────────────────\n");
    printf("\n");
    printf("  [Services management not yet implemented]\n");
    printf("\n");
    printf("  Future features:\n");
    printf("    - List system services\n");
    printf("    - Start/stop services\n");
    printf("    - View service status\n");
    printf("    - Configure service autostart\n");
}

// Render footer
void render_footer(const system_stats_t* stats) {
    char buf1[32], buf2[32], buf3[32];

    printf("\n");
    printf(COLOR_BG_WHITE COLOR_BLUE);
    printf(" Processes: %d running, %d total │ CPU: %2llu%% │ Memory: %s / %s │ Uptime: %s ",
           stats->running_processes,
           stats->total_processes,
           stats->cpu_total_usage,
           format_bytes(stats->memory_used, buf1, sizeof(buf1)),
           format_bytes(stats->memory_total, buf2, sizeof(buf2)),
           format_time(stats->uptime_seconds, buf3, sizeof(buf3)));
    printf(COLOR_RESET "\n");
}

// Render a simple ASCII graph
void render_graph(const char* title, const uint64_t* data, int samples, uint64_t max_val) {
    printf("  ");
    for (int i = 0; i < samples; i++) {
        if (i % 10 == 0) {
            printf("|");
        } else {
            printf("-");
        }
    }
    printf("\n  ");

    // Simple line graph (3 rows)
    for (int row = 0; row < 3; row++) {
        uint64_t threshold = max_val * (3 - row) / 3;
        for (int i = 0; i < samples; i++) {
            if (data[i] >= threshold) {
                printf("█");
            } else {
                printf(" ");
            }
        }
        printf("\n  ");
    }

    for (int i = 0; i < samples; i++) {
        if (i % 10 == 0) {
            printf("|");
        } else {
            printf("-");
        }
    }
    printf("\n");
}
