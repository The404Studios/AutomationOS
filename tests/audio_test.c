/**
 * Audio Subsystem Test Suite
 *
 * Tests kernel audio subsystem functionality
 */

#include "../kernel/include/audio.h"
#include "../kernel/include/hda.h"
#include "../kernel/include/mem.h"
#include "../kernel/include/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

// Test results
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        printf("TEST: %s ... ", name); \
        fflush(stdout); \
    } while(0)

#define TEST_PASS() \
    do { \
        printf("PASS\n"); \
        tests_passed++; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
        tests_failed++; \
    } while(0)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            TEST_FAIL(#cond); \
            return; \
        } \
    } while(0)

/**
 * Test device allocation
 */
static void test_device_alloc(void) {
    TEST("Device allocation");

    audio_device_t* dev = audio_device_alloc("test0", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);
    ASSERT(strcmp(dev->name, "test0") == 0);
    ASSERT(dev->type == AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev->state == AUDIO_STATE_STOPPED);
    ASSERT(dev->sample_rate == 48000);
    ASSERT(dev->channels == 2);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test format setting
 */
static void test_format_setting(void) {
    TEST("Format setting");

    audio_device_t* dev = audio_device_alloc("test1", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);

    // Test valid format
    ASSERT(audio_set_format(dev, AUDIO_FMT_S16_LE) == 0);
    ASSERT(dev->format == AUDIO_FMT_S16_LE);
    ASSERT(dev->bits_per_sample == 16);

    // Test valid channels
    ASSERT(audio_set_channels(dev, 1) == 0);
    ASSERT(dev->channels == 1);

    ASSERT(audio_set_channels(dev, 2) == 0);
    ASSERT(dev->channels == 2);

    // Test invalid channels
    ASSERT(audio_set_channels(dev, 0) == -1);
    ASSERT(audio_set_channels(dev, 3) == -1);

    // Test valid sample rates
    ASSERT(audio_set_sample_rate(dev, 44100) == 0);
    ASSERT(dev->sample_rate == 44100);

    ASSERT(audio_set_sample_rate(dev, 48000) == 0);
    ASSERT(dev->sample_rate == 48000);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test volume control
 */
static void test_volume_control(void) {
    TEST("Volume control");

    audio_device_t* dev = audio_device_alloc("test2", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);

    // Test valid volumes
    ASSERT(audio_set_volume(dev, 0) == 0);
    ASSERT(audio_get_volume(dev) == 0);

    ASSERT(audio_set_volume(dev, 50) == 0);
    ASSERT(audio_get_volume(dev) == 50);

    ASSERT(audio_set_volume(dev, 100) == 0);
    ASSERT(audio_get_volume(dev) == 100);

    // Test invalid volume
    ASSERT(audio_set_volume(dev, 101) == -1);

    // Test mute
    ASSERT(audio_set_mute(dev, true) == 0);
    ASSERT(audio_get_mute(dev) == true);

    ASSERT(audio_set_mute(dev, false) == 0);
    ASSERT(audio_get_mute(dev) == false);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test device state transitions
 */
static void test_state_transitions(void) {
    TEST("State transitions");

    audio_device_t* dev = audio_device_alloc("test3", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);

    // Initial state
    ASSERT(dev->state == AUDIO_STATE_STOPPED);

    // Reset in stopped state
    ASSERT(audio_reset(dev) == 0);
    ASSERT(dev->state == AUDIO_STATE_STOPPED);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test buffer info
 */
static void test_buffer_info(void) {
    TEST("Buffer info");

    audio_device_t* dev = audio_device_alloc("test4", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);

    audio_buf_info_t info;
    ASSERT(audio_get_buffer_info(dev, &info) == 0);

    ASSERT(info.fragments > 0);
    ASSERT(info.fragsize > 0);
    ASSERT(info.bytes >= 0);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test IOCTL handling
 */
static void test_ioctl_handling(void) {
    TEST("IOCTL handling");

    audio_device_t* dev = audio_device_alloc("test5", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);

    // Test reset
    ASSERT(audio_ioctl(dev, SNDCTL_DSP_RESET, NULL) == 0);

    // Test get block size
    uint32_t blksize = 0;
    ASSERT(audio_ioctl(dev, SNDCTL_DSP_GETBLKSIZE, &blksize) == 0);
    ASSERT(blksize > 0);

    // Test get capabilities
    uint32_t caps = 0;
    ASSERT(audio_ioctl(dev, SNDCTL_DSP_GETCAPS, &caps) == 0);
    ASSERT(caps & DSP_CAP_REALTIME);

    // Test set speed
    uint32_t speed = 48000;
    ASSERT(audio_ioctl(dev, SNDCTL_DSP_SPEED, &speed) == 0);
    ASSERT(speed == 48000);

    // Test set channels
    uint32_t channels = 2;
    ASSERT(audio_ioctl(dev, SNDCTL_DSP_CHANNELS, &channels) == 0);
    ASSERT(channels == 2);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test reference counting
 */
static void test_refcounting(void) {
    TEST("Reference counting");

    audio_device_t* dev = audio_device_alloc("test6", AUDIO_DEV_TYPE_PLAYBACK);
    ASSERT(dev != NULL);
    ASSERT(dev->refcount == 1);

    audio_open(dev);
    ASSERT(dev->refcount == 2);

    audio_close(dev);
    ASSERT(dev->refcount == 1);

    audio_device_free(dev);
    TEST_PASS();
}

/**
 * Test multiple devices
 */
static void test_multiple_devices(void) {
    TEST("Multiple devices");

    audio_device_t* dev1 = audio_device_alloc("test7a", AUDIO_DEV_TYPE_PLAYBACK);
    audio_device_t* dev2 = audio_device_alloc("test7b", AUDIO_DEV_TYPE_PLAYBACK);

    ASSERT(dev1 != NULL);
    ASSERT(dev2 != NULL);
    ASSERT(dev1 != dev2);

    ASSERT(strcmp(dev1->name, "test7a") == 0);
    ASSERT(strcmp(dev2->name, "test7b") == 0);

    audio_device_free(dev1);
    audio_device_free(dev2);
    TEST_PASS();
}

/**
 * Generate test WAV file
 */
static void* generate_test_wav(size_t* out_size) {
    // Generate 1 second of 440 Hz tone, 48 kHz, 16-bit, stereo
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t bits_per_sample = 16;
    uint32_t duration_samples = sample_rate;  // 1 second

    uint32_t audio_size = duration_samples * channels * (bits_per_sample / 8);
    uint32_t file_size = 44 + audio_size;  // WAV header is 44 bytes

    void* buffer = malloc(file_size);
    if (!buffer) {
        return NULL;
    }

    uint8_t* ptr = (uint8_t*)buffer;

    // RIFF header
    memcpy(ptr, "RIFF", 4); ptr += 4;
    *(uint32_t*)ptr = file_size - 8; ptr += 4;
    memcpy(ptr, "WAVE", 4); ptr += 4;

    // fmt chunk
    memcpy(ptr, "fmt ", 4); ptr += 4;
    *(uint32_t*)ptr = 16; ptr += 4;  // Chunk size
    *(uint16_t*)ptr = 1; ptr += 2;   // Audio format (PCM)
    *(uint16_t*)ptr = channels; ptr += 2;
    *(uint32_t*)ptr = sample_rate; ptr += 4;
    *(uint32_t*)ptr = sample_rate * channels * (bits_per_sample / 8); ptr += 4;
    *(uint16_t*)ptr = channels * (bits_per_sample / 8); ptr += 2;
    *(uint16_t*)ptr = bits_per_sample; ptr += 2;

    // data chunk
    memcpy(ptr, "data", 4); ptr += 4;
    *(uint32_t*)ptr = audio_size; ptr += 4;

    // Generate sine wave
    int16_t* samples = (int16_t*)ptr;
    for (uint32_t i = 0; i < duration_samples; i++) {
        // Simple sine approximation
        uint64_t phase = ((uint64_t)i * 440 * 65536ULL) / sample_rate;
        phase = phase % 65536;

        int32_t value;
        if (phase < 16384) {
            value = (phase * 32767) / 16384;
        } else if (phase < 32768) {
            value = 32767 - ((phase - 16384) * 32767) / 16384;
        } else if (phase < 49152) {
            value = -((phase - 32768) * 32767) / 16384;
        } else {
            value = -32767 + ((phase - 49152) * 32767) / 16384;
        }

        samples[i * 2] = (int16_t)value;      // Left
        samples[i * 2 + 1] = (int16_t)value;  // Right
    }

    *out_size = file_size;
    return buffer;
}

/**
 * Run all tests
 */
int main(void) {
    printf("=================================\n");
    printf("Audio Subsystem Test Suite\n");
    printf("=================================\n\n");

    test_device_alloc();
    test_format_setting();
    test_volume_control();
    test_state_transitions();
    test_buffer_info();
    test_ioctl_handling();
    test_refcounting();
    test_multiple_devices();

    printf("\n=================================\n");
    printf("Test Results\n");
    printf("=================================\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\nSome tests failed!\n");
        return 1;
    }
}
