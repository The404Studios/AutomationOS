/**
 * AutomationOS Terminal v1.0
 * Simple interactive command-line interface
 */

#include <stdio.h>
#include <string.h>

int main(void) {
    printf("AutomationOS Terminal v1.0\n");
    printf("Type 'help' for commands\n\n");

    char cmd[128];
    while (1) {
        printf("$ ");
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;

        cmd[strcspn(cmd, "\n")] = 0;  // Remove newline

        if (strcmp(cmd, "help") == 0) {
            printf("Commands: help, echo, ls, exit\n");
        } else if (strcmp(cmd, "exit") == 0) {
            break;
        } else if (strncmp(cmd, "echo ", 5) == 0) {
            printf("%s\n", cmd + 5);
        } else if (strcmp(cmd, "ls") == 0) {
            printf("sbin/ bin/ lib/ etc/\n");
        } else {
            printf("Unknown command: %s\n", cmd);
        }
    }

    return 0;
}
