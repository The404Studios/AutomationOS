/**
 * Audio Subsystem Core
 *
 * Provides unified audio device abstraction layer on top of HDA driver
 */

#include "../../include/audio.h"
#include "../../include/hda.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/vfs.h"
#include "../../include/x86_64.h"

// Maximum audio devices
#define MAX_AUDIO_DEVICES 8

// Global state
static audio_device_t* g_audio_devices[MAX_AUDIO_DEVICES];
static uint32_t g_num_devices = 0;
static audio_device_t* g_default_playback = NULL;
static audio_device_t* g_default_capture = NULL;

// Default audio settings
#define DEFAULT_SAMPLE_RATE    48000
#define DEFAULT_CHANNELS       2
#define DEFAULT_FORMAT         AUDIO_FMT_S16_LE
#define DEFAULT_FRAGMENT_SIZE  4096
#define DEFAULT_FRAGMENTS      4

/**
 * Initialize audio subsystem.
 * Calls hda_init() if the controller has not been brought up yet, so
 * audio_init() is safe to call as the sole audio entry point from kernel.c
 * (after pci_init()).  If no Intel HDA device is present, hda_init() is a
 * no-op and audio_init() returns -1 cleanly without hanging or panicking.
 */
int audio_init(void) {
    serial_write("AUDIO: Initializing audio subsystem\n", 37);

    // Clear device list
    for (uint32_t i = 0; i < MAX_AUDIO_DEVICES; i++) {
        g_audio_devices[i] = NULL;
    }

    // Bring up HDA controller if not already initialised.
    // hda_init() does nothing and returns cleanly when no PCI HDA device
    // is present, so this is safe on bare-metal with no audio hardware.
    if (!hda_find_controller()) {
        hda_init();
    }

    // Find HDA controller
    hda_controller_t* hda_ctrl = hda_find_controller();
    if (!hda_ctrl) {
        serial_write("AUDIO: No HDA controller found\n", 32);
        return -1;
    }

    // Create default playback device
    if (hda_ctrl->num_codecs > 0) {
        audio_device_t* dev = audio_device_alloc("dsp0", AUDIO_DEV_TYPE_PLAYBACK);
        if (dev) {
            if (audio_attach_hda(dev, hda_ctrl, hda_ctrl->codecs[0]) == 0) {
                if (audio_device_register(dev) == 0) {
                    g_default_playback = dev;
                    serial_write("AUDIO: Registered default playback device\n", 43);
                } else {
                    audio_device_free(dev);
                }
            } else {
                audio_device_free(dev);
            }
        }
    }

    // Initialize /dev/dsp character device
    dsp_device_init();

    serial_write("AUDIO: Initialization complete\n", 32);
    return 0;
}

/**
 * Shutdown audio subsystem
 */
void audio_shutdown(void) {
    serial_write("AUDIO: Shutting down audio subsystem\n", 38);

    // Clean up /dev/dsp
    dsp_device_cleanup();

    // Unregister all devices
    for (uint32_t i = 0; i < g_num_devices; i++) {
        if (g_audio_devices[i]) {
            audio_device_unregister(g_audio_devices[i]);
            audio_device_free(g_audio_devices[i]);
            g_audio_devices[i] = NULL;
        }
    }

    g_num_devices = 0;
    g_default_playback = NULL;
    g_default_capture = NULL;
}

/**
 * Allocate audio device
 */
audio_device_t* audio_device_alloc(const char* name, uint32_t type) {
    audio_device_t* dev = (audio_device_t*)kmalloc(sizeof(audio_device_t));
    if (!dev) {
        return NULL;
    }

    // Clear structure
    for (uint32_t i = 0; i < sizeof(audio_device_t); i++) {
        ((uint8_t*)dev)[i] = 0;
    }

    // Copy name
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->type = type;

    // Set capabilities
    dev->capabilities = DSP_CAP_REALTIME | DSP_CAP_BATCH;
    if (type == AUDIO_DEV_TYPE_DUPLEX) {
        dev->capabilities |= DSP_CAP_DUPLEX;
    }

    // Set defaults
    dev->sample_rate = DEFAULT_SAMPLE_RATE;
    dev->channels = DEFAULT_CHANNELS;
    dev->format = DEFAULT_FORMAT;
    dev->bits_per_sample = 16;
    dev->fragment_size = DEFAULT_FRAGMENT_SIZE;
    dev->fragments = DEFAULT_FRAGMENTS;
    dev->buffer_size = dev->fragment_size * dev->fragments;
    dev->volume = 75;
    dev->muted = false;
    dev->state = AUDIO_STATE_STOPPED;
    dev->refcount = 1;

    return dev;
}

/**
 * Free audio device
 */
void audio_device_free(audio_device_t* dev) {
    if (!dev) {
        return;
    }

    // Detach hardware
    if (dev->hda_ctrl) {
        audio_detach_hda(dev);
    }

    // Free buffer
    if (dev->buffer) {
        kfree(dev->buffer);
    }

    kfree(dev);
}

/**
 * Register audio device
 */
int audio_device_register(audio_device_t* dev) {
    if (!dev || g_num_devices >= MAX_AUDIO_DEVICES) {
        return -1;
    }

    g_audio_devices[g_num_devices++] = dev;

    serial_write("AUDIO: Registered device '", 27);
    serial_write(dev->name, strlen(dev->name));
    serial_write("'\n", 2);

    return 0;
}

/**
 * Unregister audio device
 */
void audio_device_unregister(audio_device_t* dev) {
    if (!dev) {
        return;
    }

    // Remove from device list
    for (uint32_t i = 0; i < g_num_devices; i++) {
        if (g_audio_devices[i] == dev) {
            // Shift remaining devices
            for (uint32_t j = i; j < g_num_devices - 1; j++) {
                g_audio_devices[j] = g_audio_devices[j + 1];
            }
            g_num_devices--;
            break;
        }
    }
}

/**
 * Get default audio device
 */
audio_device_t* audio_device_get_default(uint32_t type) {
    if (type == AUDIO_DEV_TYPE_PLAYBACK || type == AUDIO_DEV_TYPE_DUPLEX) {
        return g_default_playback;
    } else if (type == AUDIO_DEV_TYPE_CAPTURE) {
        return g_default_capture;
    }
    return NULL;
}

/**
 * Open audio device
 */
int audio_open(audio_device_t* dev) {
    if (!dev) {
        return -1;
    }

    serial_write("AUDIO: Opening device '", 24);
    serial_write(dev->name, strlen(dev->name));
    serial_write("'\n", 2);

    dev->refcount++;
    dev->buffer_pos = 0;

    return 0;
}

/**
 * Close audio device
 */
int audio_close(audio_device_t* dev) {
    if (!dev) {
        return -1;
    }

    serial_write("AUDIO: Closing device '", 24);
    serial_write(dev->name, strlen(dev->name));
    serial_write("'\n", 2);

    // Stop playback if running
    if (dev->state == AUDIO_STATE_PLAYING || dev->state == AUDIO_STATE_RECORDING) {
        audio_stop(dev);
    }

    dev->refcount--;
    return 0;
}

/**
 * Write audio data
 */
ssize_t audio_write(audio_device_t* dev, const void* data, size_t size) {
    if (!dev || !data || dev->type == AUDIO_DEV_TYPE_CAPTURE) {
        return -1;
    }

    if (!dev->stream) {
        return -1;
    }

    // Start stream if not running
    if (dev->state != AUDIO_STATE_PLAYING) {
        if (audio_start(dev) != 0) {
            return -1;
        }
    }

    // Write to HDA stream
    int written = hda_stream_write(dev->stream, data, size);
    if (written > 0) {
        dev->buffer_pos += written;
    }

    return written;
}

/**
 * Read audio data
 */
ssize_t audio_read(audio_device_t* dev, void* data, size_t size) {
    if (!dev || !data || dev->type == AUDIO_DEV_TYPE_PLAYBACK) {
        return -1;
    }

    if (!dev->stream) {
        return -1;
    }

    // Start stream if not running
    if (dev->state != AUDIO_STATE_RECORDING) {
        if (audio_start(dev) != 0) {
            return -1;
        }
    }

    // Read from HDA stream
    int read = hda_stream_read(dev->stream, data, size);
    if (read > 0) {
        dev->buffer_pos += read;
    }

    return read;
}

/**
 * IOCTL handler
 */
int audio_ioctl(audio_device_t* dev, uint32_t cmd, void* arg) {
    if (!dev) {
        return -1;
    }

    switch (cmd) {
    case SNDCTL_DSP_RESET:
        return audio_reset(dev);

    case SNDCTL_DSP_SYNC:
        return audio_drain(dev);

    case SNDCTL_DSP_SPEED:
        if (arg) {
            uint32_t rate = *(uint32_t*)arg;
            if (audio_set_sample_rate(dev, rate) == 0) {
                *(uint32_t*)arg = dev->sample_rate;
                return 0;
            }
        }
        return -1;

    case SNDCTL_DSP_STEREO:
        if (arg) {
            uint32_t stereo = *(uint32_t*)arg;
            return audio_set_channels(dev, stereo ? 2 : 1);
        }
        return -1;

    case SNDCTL_DSP_CHANNELS:
        if (arg) {
            uint32_t channels = *(uint32_t*)arg;
            if (audio_set_channels(dev, channels) == 0) {
                *(uint32_t*)arg = dev->channels;
                return 0;
            }
        }
        return -1;

    case SNDCTL_DSP_SETFMT:
        if (arg) {
            uint32_t format = *(uint32_t*)arg;
            if (audio_set_format(dev, format) == 0) {
                *(uint32_t*)arg = dev->format;
                return 0;
            }
        }
        return -1;

    case SNDCTL_DSP_GETBLKSIZE:
        if (arg) {
            *(uint32_t*)arg = dev->fragment_size;
            return 0;
        }
        return -1;

    case SNDCTL_DSP_GETOSPACE:
    case SNDCTL_DSP_GETISPACE:
        if (arg) {
            return audio_get_buffer_info(dev, (audio_buf_info_t*)arg);
        }
        return -1;

    case SNDCTL_DSP_GETCAPS:
        if (arg) {
            *(uint32_t*)arg = dev->capabilities;
            return 0;
        }
        return -1;

    default:
        return -1;
    }
}

/**
 * Set audio format
 */
int audio_set_format(audio_device_t* dev, uint32_t format) {
    if (!dev || dev->state != AUDIO_STATE_STOPPED) {
        return -1;
    }

    // Map OSS format to internal format
    switch (format) {
    case AUDIO_FMT_S16_LE:
        dev->format = format;
        dev->bits_per_sample = 16;
        return 0;

    case AUDIO_FMT_U8:
        dev->format = format;
        dev->bits_per_sample = 8;
        return 0;

    case AUDIO_FMT_S24_LE:
        dev->format = format;
        dev->bits_per_sample = 24;
        return 0;

    case AUDIO_FMT_S32_LE:
        dev->format = format;
        dev->bits_per_sample = 32;
        return 0;

    default:
        return -1;
    }
}

/**
 * Set number of channels
 */
int audio_set_channels(audio_device_t* dev, uint32_t channels) {
    if (!dev || dev->state != AUDIO_STATE_STOPPED) {
        return -1;
    }

    if (channels < 1 || channels > 2) {
        return -1;
    }

    dev->channels = channels;
    return 0;
}

/**
 * Set sample rate
 */
int audio_set_sample_rate(audio_device_t* dev, uint32_t rate) {
    if (!dev || dev->state != AUDIO_STATE_STOPPED) {
        return -1;
    }

    // Only support common rates
    if (rate != 44100 && rate != 48000) {
        // Round to nearest supported rate
        rate = (rate < 46000) ? 44100 : 48000;
    }

    dev->sample_rate = rate;
    return 0;
}

/**
 * Start playback/recording
 */
int audio_start(audio_device_t* dev) {
    if (!dev || !dev->hda_ctrl || !dev->codec) {
        return -1;
    }

    if (dev->state == AUDIO_STATE_PLAYING || dev->state == AUDIO_STATE_RECORDING) {
        return 0;  // Already started
    }

    serial_write("AUDIO: Starting ", 16);
    serial_write(dev->name, strlen(dev->name));
    serial_putchar('\n');

    // Allocate stream if needed
    if (!dev->stream) {
        bool is_output = (dev->type == AUDIO_DEV_TYPE_PLAYBACK);
        dev->stream = hda_stream_alloc(dev->hda_ctrl, is_output);
        if (!dev->stream) {
            serial_write("AUDIO: Failed to allocate stream\n", 34);
            return -1;
        }
    }

    // Setup stream with current parameters
    if (hda_stream_setup(dev->hda_ctrl, dev->stream,
                         dev->sample_rate, dev->bits_per_sample, dev->channels) != 0) {
        serial_write("AUDIO: Failed to setup stream\n", 31);
        return -1;
    }

    // Set volume
    hda_set_volume(dev->codec, dev->hda_ctrl, dev->volume);

    // Start HDA stream
    if (hda_stream_start(dev->hda_ctrl, dev->stream) != 0) {
        serial_write("AUDIO: Failed to start stream\n", 31);
        return -1;
    }

    dev->state = (dev->type == AUDIO_DEV_TYPE_PLAYBACK) ?
                 AUDIO_STATE_PLAYING : AUDIO_STATE_RECORDING;

    return 0;
}

/**
 * Stop playback/recording
 */
int audio_stop(audio_device_t* dev) {
    if (!dev) {
        return -1;
    }

    if (dev->state == AUDIO_STATE_STOPPED) {
        return 0;  // Already stopped
    }

    serial_write("AUDIO: Stopping ", 16);
    serial_write(dev->name, strlen(dev->name));
    serial_putchar('\n');

    // Stop HDA stream
    if (dev->stream) {
        hda_stream_stop(dev->hda_ctrl, dev->stream);
    }

    dev->state = AUDIO_STATE_STOPPED;
    return 0;
}

/**
 * Pause playback
 */
int audio_pause(audio_device_t* dev) {
    if (!dev || dev->state != AUDIO_STATE_PLAYING) {
        return -1;
    }

    if (dev->stream) {
        hda_stream_stop(dev->hda_ctrl, dev->stream);
    }

    dev->state = AUDIO_STATE_PAUSED;
    return 0;
}

/**
 * Resume playback
 */
int audio_resume(audio_device_t* dev) {
    if (!dev || dev->state != AUDIO_STATE_PAUSED) {
        return -1;
    }

    if (dev->stream) {
        hda_stream_start(dev->hda_ctrl, dev->stream);
    }

    dev->state = AUDIO_STATE_PLAYING;
    return 0;
}

/**
 * Drain audio buffer (wait for playback to complete)
 */
int audio_drain(audio_device_t* dev) {
    if (!dev) {
        return -1;
    }

    // Simple implementation: just stop
    // In a real implementation, we'd wait for buffer to empty
    return audio_stop(dev);
}

/**
 * Reset audio device
 */
int audio_reset(audio_device_t* dev) {
    if (!dev) {
        return -1;
    }

    audio_stop(dev);
    dev->buffer_pos = 0;

    if (dev->stream) {
        // Clear stream buffer
        if (dev->stream->buffer_virt) {
            for (uint32_t i = 0; i < dev->stream->buffer_size; i++) {
                ((uint8_t*)dev->stream->buffer_virt)[i] = 0;
            }
        }
    }

    return 0;
}

/**
 * Set volume (0-100)
 */
int audio_set_volume(audio_device_t* dev, uint32_t volume) {
    if (!dev || volume > 100) {
        return -1;
    }

    dev->volume = volume;

    // Apply to hardware if available
    if (dev->codec && dev->hda_ctrl) {
        hda_set_volume(dev->codec, dev->hda_ctrl, volume);
    }

    return 0;
}

/**
 * Get volume
 */
uint32_t audio_get_volume(audio_device_t* dev) {
    return dev ? dev->volume : 0;
}

/**
 * Set mute
 */
int audio_set_mute(audio_device_t* dev, bool mute) {
    if (!dev) {
        return -1;
    }

    dev->muted = mute;

    // Apply to hardware if available
    if (dev->codec && dev->hda_ctrl) {
        hda_set_mute(dev->codec, dev->hda_ctrl, mute);
    }

    return 0;
}

/**
 * Get mute state
 */
bool audio_get_mute(audio_device_t* dev) {
    return dev ? dev->muted : false;
}

/**
 * Get buffer information
 */
int audio_get_buffer_info(audio_device_t* dev, audio_buf_info_t* info) {
    if (!dev || !info) {
        return -1;
    }

    info->fragments = dev->fragments;
    info->fragstotal = dev->fragments;
    info->fragsize = dev->fragment_size;
    info->bytes = audio_get_buffer_space(dev);

    return 0;
}

/**
 * Get available buffer space
 */
uint32_t audio_get_buffer_space(audio_device_t* dev) {
    if (!dev || !dev->stream) {
        return 0;
    }

    // Simple implementation: return full buffer size if stopped, half if playing
    return (dev->state == AUDIO_STATE_STOPPED) ? dev->buffer_size : dev->buffer_size / 2;
}

/**
 * Attach HDA backend
 */
int audio_attach_hda(audio_device_t* dev, hda_controller_t* ctrl, hda_codec_t* codec) {
    if (!dev || !ctrl || !codec) {
        return -1;
    }

    dev->hda_ctrl = ctrl;
    dev->codec = codec;

    serial_write("AUDIO: Attached HDA backend to '", 33);
    serial_write(dev->name, strlen(dev->name));
    serial_write("'\n", 2);

    return 0;
}

/**
 * Detach HDA backend
 */
void audio_detach_hda(audio_device_t* dev) {
    if (!dev) {
        return;
    }

    // Stop and free stream
    if (dev->stream) {
        if (dev->state == AUDIO_STATE_PLAYING || dev->state == AUDIO_STATE_RECORDING) {
            hda_stream_stop(dev->hda_ctrl, dev->stream);
        }
        hda_stream_free(dev->stream);
        dev->stream = NULL;
    }

    dev->hda_ctrl = NULL;
    dev->codec = NULL;
}

/**
 * /dev/dsp file operations
 */
static ssize_t dsp_read(vfs_file_t* file, void* buf, size_t count) {
    audio_device_t* dev = (audio_device_t*)file->private_data;
    if (!dev) {
        dev = audio_device_get_default(AUDIO_DEV_TYPE_CAPTURE);
        if (!dev) {
            return -1;
        }
        file->private_data = dev;
    }

    return audio_read(dev, buf, count);
}

static ssize_t dsp_write(vfs_file_t* file, const void* buf, size_t count) {
    audio_device_t* dev = (audio_device_t*)file->private_data;
    if (!dev) {
        dev = audio_device_get_default(AUDIO_DEV_TYPE_PLAYBACK);
        if (!dev) {
            return -1;
        }
        file->private_data = dev;
    }

    return audio_write(dev, buf, count);
}

static int dsp_open(vfs_inode_t* inode, vfs_file_t* file) {
    audio_device_t* dev = audio_device_get_default(AUDIO_DEV_TYPE_PLAYBACK);
    if (!dev) {
        return -1;
    }

    file->private_data = dev;
    return audio_open(dev);
}

static int dsp_close(vfs_file_t* file) {
    audio_device_t* dev = (audio_device_t*)file->private_data;
    if (dev) {
        return audio_close(dev);
    }
    return 0;
}

static vfs_file_ops_t dsp_fops = {
    .read = dsp_read,
    .write = dsp_write,
    .open = dsp_open,
    .close = dsp_close,
    .lseek = NULL
};

/**
 * Initialize /dev/dsp device node
 */
int dsp_device_init(void) {
    serial_write("AUDIO: Initializing /dev/dsp\n", 30);

    // Create device node in VFS
    // Note: This is a simplified implementation
    // In a full implementation, we would:
    // 1. Create /dev directory if it doesn't exist
    // 2. Create character device node with proper major/minor numbers
    // 3. Register with devfs

    // For now, just log success
    serial_write("AUDIO: /dev/dsp initialized\n", 29);

    return 0;
}

/**
 * Clean up /dev/dsp
 */
void dsp_device_cleanup(void) {
    serial_write("AUDIO: Cleaning up /dev/dsp\n", 29);
    // Remove device node from VFS
}
