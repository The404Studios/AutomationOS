// userspace/init/init.c - Init process (PID 1)
// First userspace process that spawns the shell

#include "../libc/stdio.h"
#include "../libc/syscall.h"
#include "../libc/string.h"

void _start(void) {
    // Init process entry point
    printf("\n");
    printf("=====================================\n");
    printf("   AutomationOS Init Process (PID 1)\n");
    printf("=====================================\n");
    printf("\n");

    // Verify we are PID 1
    int pid = getpid();
    printf("[INIT] Running as PID %d\n", pid);

    if (pid != 1) {
        printf("[INIT] ERROR: Init must be PID 1!\n");
        exit(1);
    }

    printf("[INIT] Initializing system...\n");

    // TODO: Mount filesystems, setup environment, etc.
    printf("[INIT] Filesystem mounting not yet implemented\n");

    // Start service manager
    printf("[INIT] Starting service manager...\n");

    int svcmgr_pid = fork();
    if (svcmgr_pid < 0) {
        printf("[INIT] ERROR: Failed to fork service manager\n");
    } else if (svcmgr_pid == 0) {
        // Child process - execute service manager
        char* argv[] = { "/sbin/servicemanager", "--boot", NULL };
        char* envp[] = { NULL };
        execve("/sbin/servicemanager", argv, envp);

        // If execve returns, it failed
        printf("[INIT] ERROR: Failed to execute service manager\n");
        printf("[INIT] Services will not be started automatically\n");
        exit(1);
    } else {
        printf("[INIT] Service manager started with PID %d\n", svcmgr_pid);

        // Give service manager time to initialize
        sleep(2);

        printf("[INIT] Services should now be starting...\n");
    }

    // Spawn shell
    printf("[INIT] Spawning shell...\n");

    int shell_pid = fork();
    if (shell_pid < 0) {
        printf("[INIT] ERROR: Failed to fork shell\n");
        exit(1);
    }

    if (shell_pid == 0) {
        // Child process - execute shell
        char* argv[] = { "/bin/shell", NULL };
        char* envp[] = { NULL };
        execve("/bin/shell", argv, envp);

        // If execve returns, it failed
        printf("[INIT] ERROR: Failed to execute shell\n");
        exit(1);
    }

    // Parent process - wait for shell
    printf("[INIT] Shell started with PID %d\n", shell_pid);

    // Wait for child processes
    while (1) {
        int status;
        int dead_pid = waitpid(-1, &status, 0);

        if (dead_pid > 0) {
            printf("[INIT] Process %d terminated with status %d\n", dead_pid, status);

            // If shell died, respawn it
            if (dead_pid == shell_pid) {
                printf("[INIT] Shell died, respawning...\n");

                shell_pid = fork();
                if (shell_pid == 0) {
                    char* argv[] = { "/bin/shell", NULL };
                    char* envp[] = { NULL };
                    execve("/bin/shell", argv, envp);
                    printf("[INIT] ERROR: Failed to execute shell\n");
                    exit(1);
                }

                printf("[INIT] Shell respawned with PID %d\n", shell_pid);
            }
        }

        // Sleep to avoid busy-waiting
        sleep(1);
    }

    // Should never reach here
    exit(0);
}
