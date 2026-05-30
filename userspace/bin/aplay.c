/**
 * aplay - Audio Player Utility
 *
 * Simple command-line WAV file player
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "../lib/audio/audio.h"

#define PROGRAM_NAME "aplay"
#define VERSION "1.0"

static void print_usage(const char* progname) {
    printf("Usage: %s [options] <file.wav>\n", progname);
    printf("\n");
    printf("Options:\n");
    printf("  -D, --device=NAME    Audio device to use (default: /dev/dsp)\n");
    printf("  -v, --volume=N       Set volume (0-100, default: 75)\n");
    printf("  -h, --help           Display this help\n");
    printf("  -V, --version        Display version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s music.wav           Play music.wav\n", progname);
    printf("  %s -v 50 sound.wav     Play at 50%% volume\n", progname);
    printf("  %s -D /dev/dsp1 a.wav  Play on specific device\n", progname);
    printf("\n");
}

static void print_version(void) {
    printf("%s version %s\n", PROGRAM_NAME, VERSION);
    printf("Simple WAV file player for AutomationOS\n");
}

static void print_wav_info(const wav_info_t* info) {
    printf("Playing WAV file:\n");
    printf("  Sample rate:  %u Hz\n", info->sample_rate);
    printf("  Channels:     %u (%s)\n", info->channels,
           info->channels == 1 ? "mono" : info->channels == 2 ? "stereo" : "multi-channel");
    printf("  Bit depth:    %u-bit\n", info->bits_per_sample);
    printf("  Data size:    %u bytes\n", info->data_size);

    // Calculate duration
    uint32_t bytes_per_sec = info->sample_rate * info->channels * (info->bits_per_sample / 8);
    if (bytes_per_sec > 0) {
        uint32_t duration_sec = info->data_size / bytes_per_sec;
        uint32_t duration_ms = ((info->data_size % bytes_per_sec) * 1000) / bytes_per_sec;
        printf("  Duration:     %u.%03u seconds\n", duration_sec, duration_ms);
    }
    printf("\n");
}

int main(int argc, char* argv[]) {
    const char* device = "/dev/dsp";
    uint32_t volume = 75;
    int opt;

    static struct option long_options[] = {
        {"device",  required_argument, 0, 'D'},
        {"volume",  required_argument, 0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    // Parse command line options
    while ((opt = getopt_long(argc, argv, "D:v:hV", long_options, NULL)) != -1) {
        switch (opt) {
        case 'D':
            device = optarg;
            break;

        case 'v':
            volume = atoi(optarg);
            if (volume > 100) {
                fprintf(stderr, "Error: Volume must be 0-100\n");
                return 1;
            }
            break;

        case 'h':
            print_usage(argv[0]);
            return 0;

        case 'V':
            print_version();
            return 0;

        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    // Check for filename argument
    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    const char* filename = argv[optind];

    // Open audio device
    printf("Opening audio device: %s\n", device);
    audio_t* audio = audio_open(device);
    if (!audio) {
        fprintf(stderr, "Error: Failed to open audio device: %s\n", audio_get_error());
        return 1;
    }

    // Set volume
    if (audio_set_volume(audio, volume) == 0) {
        printf("Volume set to %u%%\n", volume);
    }

    // Open WAV file
    printf("Loading: %s\n", filename);
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open file '%s'\n", filename);
        audio_close(audio);
        return 1;
    }

    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        fprintf(stderr, "Error: Failed to get file size\n");
        close(fd);
        audio_close(audio);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    // Allocate buffer
    void* buffer = malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate buffer (%ld bytes)\n", (long)file_size);
        close(fd);
        audio_close(audio);
        return 1;
    }

    // Read file
    ssize_t bytes_read = read(fd, buffer, file_size);
    close(fd);

    if (bytes_read != file_size) {
        fprintf(stderr, "Error: Failed to read file\n");
        free(buffer);
        audio_close(audio);
        return 1;
    }

    // Parse WAV header
    wav_info_t info;
    const void* audio_data;

    if (audio_wav_parse(buffer, file_size, &info, &audio_data) != 0) {
        fprintf(stderr, "Error: Invalid WAV file: %s\n", audio_get_error());
        free(buffer);
        audio_close(audio);
        return 1;
    }

    // Print WAV info
    print_wav_info(&info);

    // Set audio format
    printf("Configuring audio device...\n");
    if (audio_set_format(audio, info.sample_rate, info.channels, info.format) != 0) {
        fprintf(stderr, "Error: Failed to configure audio: %s\n", audio_get_error());
        free(buffer);
        audio_close(audio);
        return 1;
    }

    // Play audio
    printf("Playing...\n");
    const uint8_t* ptr = (const uint8_t*)audio_data;
    size_t remaining = info.data_size;
    size_t chunk_size = 4096;

    while (remaining > 0) {
        size_t to_write = (remaining < chunk_size) ? remaining : chunk_size;
        ssize_t written = audio_write(audio, ptr, to_write);

        if (written < 0) {
            fprintf(stderr, "Error: Write failed: %s\n", audio_get_error());
            free(buffer);
            audio_close(audio);
            return 1;
        }

        ptr += written;
        remaining -= written;

        // Print progress (simple dots)
        static int dot_count = 0;
        if (++dot_count % 10 == 0) {
            printf(".");
            fflush(stdout);
        }
    }

    printf("\n");

    // Wait for playback to complete
    printf("Draining buffer...\n");
    audio_drain(audio);

    printf("Playback complete.\n");

    // Cleanup
    free(buffer);
    audio_close(audio);

    return 0;
}
