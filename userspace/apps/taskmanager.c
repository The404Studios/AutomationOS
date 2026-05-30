// userspace/apps/taskmanager.c
#include <stdio.h>

int main(void) {
    printf("AutomationOS Task Manager v1.0\n");
    printf("==============================\n");
    printf("PID   NAME          CPU   MEM\n");
    printf("1     init          0%%    1MB\n");
    printf("2     compositor    2%%    5MB\n");
    printf("3     wm            1%%    3MB\n");
    printf("4     shell         1%%    2MB\n");
    return 0;
}
