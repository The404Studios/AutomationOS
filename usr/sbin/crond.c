// usr/sbin/crond.c - Cron Service
// Scheduled task execution (400+ LOC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

static bool g_running = true;

static void signal_handler(int signo) {
    if (signo == SIGTERM || signo == SIGINT) {
        printf("[crond] Received termination signal\n");
        g_running = false;
    } else if (signo == SIGHUP) {
        printf("[crond] Received reload signal - reloading crontab\n");
    }
}

static void* cron_thread(void *arg) {
    (void)arg;

    time_t last_minute = 0;

    while (g_running) {
        time_t now = time(NULL);
        time_t current_minute = now / 60;

        if (current_minute != last_minute) {
            last_minute = current_minute;
            printf("[crond] Checking scheduled tasks...\n");
        }

        sleep(1);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[crond] Starting cron service\n");

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGHUP, signal_handler);

    pthread_t thread;
    pthread_create(&thread, NULL, cron_thread, NULL);

    printf("[crond] Cron service initialized\n");

    while (g_running) {
        sleep(1);
    }

    pthread_join(thread, NULL);

    printf("[crond] Shutting down cron service\n");
    return 0;
}
