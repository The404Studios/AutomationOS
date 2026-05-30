/**
 * audiomixerd - Simple Audio Mixer Daemon
 *
 * Provides software mixing for multiple audio streams
 * (Simplified implementation for demonstration)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../lib/audio/audio.h"

#define MIXER_SOCKET "/tmp/audiomixer.sock"
#define MAX_CLIENTS 8
#define BUFFER_SIZE 4096

typedef struct {
    int fd;
    bool active;
    uint32_t volume;
} client_t;

static volatile bool g_running = true;
static audio_t* g_audio = NULL;
static client_t g_clients[MAX_CLIENTS];

/**
 * Signal handler
 */
static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

/**
 * Initialize mixer
 */
static int mixer_init(void) {
    printf("audiomixerd: Initializing audio mixer\n");

    // Initialize client array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].active = false;
        g_clients[i].volume = 100;
    }

    // Open audio device
    g_audio = audio_open("/dev/dsp");
    if (!g_audio) {
        fprintf(stderr, "audiomixerd: Failed to open audio device: %s\n", audio_get_error());
        return -1;
    }

    // Set default format: 48kHz, stereo, 16-bit
    if (audio_set_format(g_audio, 48000, 2, AUDIO_FORMAT_S16_LE) != 0) {
        fprintf(stderr, "audiomixerd: Failed to set audio format\n");
        audio_close(g_audio);
        return -1;
    }

    printf("audiomixerd: Audio device initialized\n");
    return 0;
}

/**
 * Cleanup mixer
 */
static void mixer_cleanup(void) {
    printf("audiomixerd: Shutting down\n");

    // Close all clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].fd >= 0) {
            close(g_clients[i].fd);
        }
    }

    // Close audio device
    if (g_audio) {
        audio_close(g_audio);
    }

    // Remove socket
    unlink(MIXER_SOCKET);
}

/**
 * Mix audio samples
 */
static void mix_samples(int16_t* output, int16_t* input, size_t samples, uint32_t volume) {
    for (size_t i = 0; i < samples; i++) {
        // Simple mixing with volume scaling
        int32_t mixed = output[i] + ((input[i] * volume) / 100);

        // Clamp to 16-bit range
        if (mixed > 32767) {
            mixed = 32767;
        } else if (mixed < -32768) {
            mixed = -32768;
        }

        output[i] = (int16_t)mixed;
    }
}

/**
 * Process audio from clients
 */
static void process_audio(void) {
    int16_t mix_buffer[BUFFER_SIZE / sizeof(int16_t)];
    int16_t client_buffer[BUFFER_SIZE / sizeof(int16_t)];
    bool have_data = false;

    // Clear mix buffer
    memset(mix_buffer, 0, sizeof(mix_buffer));

    // Mix audio from all active clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active || g_clients[i].fd < 0) {
            continue;
        }

        // Try to read from client
        ssize_t bytes_read = read(g_clients[i].fd, client_buffer, sizeof(client_buffer));
        if (bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int16_t);
            mix_samples(mix_buffer, client_buffer, samples, g_clients[i].volume);
            have_data = true;
        } else if (bytes_read == 0) {
            // Client disconnected
            printf("audiomixerd: Client %d disconnected\n", i);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_clients[i].active = false;
        }
    }

    // Write mixed audio to device
    if (have_data && g_audio) {
        audio_write(g_audio, mix_buffer, sizeof(mix_buffer));
    }
}

/**
 * Main mixer loop
 */
static int mixer_run(void) {
    printf("audiomixerd: Starting mixer loop\n");

    // For this simplified implementation, we just process audio continuously
    // In a real implementation, we would:
    // 1. Accept client connections via Unix socket
    // 2. Use select/poll to multiplex between clients
    // 3. Implement proper buffering and synchronization

    while (g_running) {
        process_audio();
        usleep(10000);  // 10ms sleep
    }

    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    bool daemonize = true;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-f") == 0) {
            daemonize = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("\n");
            printf("Options:\n");
            printf("  -f, --foreground    Run in foreground\n");
            printf("  -h, --help          Display this help\n");
            printf("\n");
            return 0;
        }
    }

    // Daemonize if requested
    if (daemonize) {
        printf("audiomixerd: Daemonizing...\n");
        if (daemon(0, 0) != 0) {
            fprintf(stderr, "audiomixerd: Failed to daemonize\n");
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize mixer
    if (mixer_init() != 0) {
        return 1;
    }

    // Run mixer
    int ret = mixer_run();

    // Cleanup
    mixer_cleanup();

    return ret;
}
