// userspace/apps/taskmanager/input.c - Input handling

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/syscall.h"

// External global for exit control
extern bool g_running;

#define COLOR_RESET "\033[0m"
#define COLOR_BOLD  "\033[1m"

void show_process_details(const process_info_t* proc);

// Get a single character without waiting (non-blocking)
static int getchar_nonblock(void) {
    // TODO: Implement non-blocking read
    // For now, just use blocking getchar
    // In a real implementation, we'd use select() or poll()
    return -1;  // No input available
}

// Handle keyboard input
void handle_input(ui_state_t* ui, process_info_t* procs, int proc_count) {
    int c = getchar_nonblock();
    if (c < 0) {
        return;  // No input
    }

    switch (c) {
        // Tab switching
        case '1':
        case '\t':
            ui->current_tab = TAB_PROCESSES;
            break;

        case '2':
            ui->current_tab = TAB_PERFORMANCE;
            break;

        case '3':
            ui->current_tab = TAB_SERVICES;
            break;

        // Process list navigation
        case 'k':  // Up arrow (or 'k' vim-style)
        case 65:   // Up arrow ANSI sequence
            if (ui->selected_process > 0) {
                ui->selected_process--;
                if (ui->selected_process < ui->scroll_offset) {
                    ui->scroll_offset--;
                }
            }
            break;

        case 'j':  // Down arrow (or 'j' vim-style)
        case 66:   // Down arrow ANSI sequence
            if (ui->selected_process < (uint32_t)(proc_count - 1)) {
                ui->selected_process++;
                if (ui->selected_process >= ui->scroll_offset + 15) {
                    ui->scroll_offset++;
                }
            }
            break;

        // Sort modes
        case 'p':
        case 'P':
            ui->sort_mode = SORT_PID;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        case 'n':
        case 'N':
            ui->sort_mode = SORT_NAME;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        case 'c':
        case 'C':
            ui->sort_mode = SORT_CPU;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        case 'm':
        case 'M':
            ui->sort_mode = SORT_MEMORY;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        case 'd':
        case 'D':
            ui->sort_mode = SORT_DISK;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        case 'x':
        case 'X':
            ui->sort_mode = SORT_NETWORK;
            ui->sort_ascending = !ui->sort_ascending;
            break;

        // Process control
        case 'K':  // Kill process (uppercase K)
            if (ui->selected_process < (uint32_t)proc_count) {
                uint32_t pid = procs[ui->selected_process].pid;
                clear_screen();
                printf("\n Kill process %d (%s)? [y/N]: ",
                       pid, procs[ui->selected_process].name);
                int confirm = getchar();
                if (confirm == 'y' || confirm == 'Y') {
                    kill_process(pid, 9);  // SIGKILL
                    printf("\n Press any key to continue...");
                    getchar();
                }
            }
            break;

        case 's':
        case 'S':  // Suspend process
            if (ui->selected_process < (uint32_t)proc_count) {
                uint32_t pid = procs[ui->selected_process].pid;
                suspend_process(pid);
            }
            break;

        case 'r':
        case 'R':  // Resume process
            if (ui->selected_process < (uint32_t)proc_count) {
                uint32_t pid = procs[ui->selected_process].pid;
                resume_process(pid);
            }
            break;

        // Search filter
        case '/':
            clear_screen();
            printf("\n Search (enter to clear): ");
            // Read search string
            // TODO: Implement proper line editing
            break;

        // Toggle kernel processes
        case 'h':
        case 'H':
            ui->show_kernel_processes = !ui->show_kernel_processes;
            break;

        // Quit
        case 'q':
        case 'Q':
        case 27:  // ESC
            clear_screen();
            printf("\n Exit Task Manager? [y/N]: ");
            int confirm = getchar();
            if (confirm == 'y' || confirm == 'Y') {
                g_running = false;
            }
            break;

        // Enter - show process details
        case '\n':
        case '\r':
            if (ui->selected_process < (uint32_t)proc_count) {
                show_process_details(&procs[ui->selected_process]);
            }
            break;

        default:
            // Ignore unknown keys
            break;
    }
}

// Show detailed process information
void show_process_details(const process_info_t* proc) {
    char buf1[32], buf2[32], buf3[32], buf4[32];

    clear_screen();
    printf("\n");
    printf("════════════════════════════════════════════════════════════════════════════════\n");
    printf("                             Process Details                                    \n");
    printf("════════════════════════════════════════════════════════════════════════════════\n");
    printf("\n");

    printf(COLOR_BOLD "  Process ID:" COLOR_RESET "      %d\n", proc->pid);
    printf(COLOR_BOLD "  Parent PID:" COLOR_RESET "      %d\n", proc->parent_pid);
    printf(COLOR_BOLD "  Name:" COLOR_RESET "            %s\n", proc->name);
    printf(COLOR_BOLD "  User:" COLOR_RESET "            %s\n", proc->username);
    printf(COLOR_BOLD "  State:" COLOR_RESET "           %s\n", state_to_string(proc->state));
    printf(COLOR_BOLD "  Priority:" COLOR_RESET "        %d\n", proc->priority);
    printf(COLOR_BOLD "  CPU Affinity:" COLOR_RESET "   0x%X\n", proc->cpu_affinity);
    printf(COLOR_BOLD "  Threads:" COLOR_RESET "         %d\n", proc->threads);
    printf("\n");

    printf(COLOR_BOLD "Resource Usage:\n" COLOR_RESET);
    printf("  CPU:             %llu%%\n", proc->cpu_percent);
    printf("  Total CPU Time:  %llu ticks\n", proc->total_cpu_time);
    printf("  Memory (RSS):    %s\n", format_bytes(proc->memory_rss, buf1, sizeof(buf1)));
    printf("  Memory (Shared): %s\n", format_bytes(proc->memory_shared, buf2, sizeof(buf2)));
    printf("  Memory (Virt):   %s\n", format_bytes(proc->memory_virtual, buf3, sizeof(buf3)));
    printf("\n");

    printf(COLOR_BOLD "I/O Statistics:\n" COLOR_RESET);
    printf("  Disk Read:       %s\n", format_rate(proc->disk_read, buf1, sizeof(buf1)));
    printf("  Disk Write:      %s\n", format_rate(proc->disk_write, buf2, sizeof(buf2)));
    printf("  Network Recv:    %s\n", format_rate(proc->net_recv, buf3, sizeof(buf3)));
    printf("  Network Send:    %s\n", format_rate(proc->net_send, buf4, sizeof(buf4)));
    printf("\n");

    printf("════════════════════════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf(COLOR_BOLD "Actions:\n" COLOR_RESET);
    printf("  K - Kill process\n");
    printf("  S - Suspend process\n");
    printf("  R - Resume process\n");
    printf("  P - Change priority\n");
    printf("  A - Set CPU affinity\n");
    printf("  Q - Back to list\n");
    printf("\n");
    printf("Select action: ");

    int c = getchar();
    switch (c) {
        case 'k':
        case 'K':
            printf("\n Are you sure? [y/N]: ");
            if (getchar() == 'y') {
                kill_process(proc->pid, 9);
            }
            break;

        case 's':
        case 'S':
            suspend_process(proc->pid);
            printf("\n Press any key to continue...");
            getchar();
            break;

        case 'r':
        case 'R':
            resume_process(proc->pid);
            printf("\n Press any key to continue...");
            getchar();
            break;

        case 'p':
        case 'P':
            printf("\n Enter new priority (-20 to 19): ");
            // TODO: Read integer input
            // For now, just return
            break;

        case 'a':
        case 'A':
            printf("\n Enter CPU affinity mask (hex): 0x");
            // TODO: Read hex input
            break;

        default:
            break;
    }
}
