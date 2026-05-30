/**
 * Audio Subsystem Header
 *
 * Provides kernel-level audio device abstraction and /dev/dsp interface
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"
#include "hda.h"

// Audio device types
#define AUDIO_DEV_TYPE_PLAYBACK    0x01
#define AUDIO_DEV_TYPE_CAPTURE     0x02
#define AUDIO_DEV_TYPE_DUPLEX      0x03

// Audio formats
#define AUDIO_FMT_U8               0x00
#define AUDIO_FMT_S16_LE           0x01
#define AUDIO_FMT_S24_LE           0x02
#define AUDIO_FMT_S32_LE           0x03

// Audio states
#define AUDIO_STATE_STOPPED        0
#define AUDIO_STATE_PLAYING        1
#define AUDIO_STATE_PAUSED         2
#define AUDIO_STATE_RECORDING      3

// IOCTLs (OSS-compatible)
#define SNDCTL_DSP_RESET           0x5000
#define SNDCTL_DSP_SYNC            0x5001
#define SNDCTL_DSP_SPEED           0x5002
#define SNDCTL_DSP_STEREO          0x5003
#define SNDCTL_DSP_GETBLKSIZE      0x5004
#define SNDCTL_DSP_SETFMT          0x5005
#define SNDCTL_DSP_CHANNELS        0x5006
#define SNDCTL_DSP_POST            0x5008
#define SNDCTL_DSP_GETOSPACE       0x500C
#define SNDCTL_DSP_GETISPACE       0x500D
#define SNDCTL_DSP_SETFRAGMENT     0x500A
#define SNDCTL_DSP_GETCAPS         0x500F

// Audio device capabilities
#define DSP_CAP_REVISION           0x000000FF
#define DSP_CAP_DUPLEX             0x00000100
#define DSP_CAP_REALTIME           0x00000200
#define DSP_CAP_BATCH              0x00000400
#define DSP_CAP_COPROC             0x00000800
#define DSP_CAP_TRIGGER            0x00001000
#define DSP_CAP_MMAP               0x00002000

// Audio buffer info
typedef struct {
    uint32_t fragments;      // Total number of fragments
    uint32_t fragstotal;     // Total fragments
    uint32_t fragsize;       // Size of each fragment
    uint32_t bytes;          // Total bytes available
} audio_buf_info_t;

// Audio device structure
typedef struct audio_device {
    char name[64];
    uint32_t type;           // Playback, capture, or duplex
    uint32_t capabilities;

    // Hardware backend
    hda_controller_t* hda_ctrl;
    hda_codec_t* codec;
    hda_stream_t* stream;

    // Current format
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t bits_per_sample;

    // Buffer management
    void* buffer;
    uint32_t buffer_size;
    uint32_t fragment_size;
    uint32_t fragments;
    uint32_t buffer_pos;

    // State
    uint32_t state;
    uint32_t volume;         // 0-100
    bool muted;

    // Reference counting
    uint32_t refcount;

    // VFS integration
    void* vfs_private;
} audio_device_t;

// Audio subsystem initialization
int audio_init(void);
void audio_shutdown(void);

// Minimal one-shot tone / PCM playback API (kernel/drivers/audio/audio_tone.c).
// These sit directly on the HDA stream driver and require hda_init() to have
// brought the controller + codec up first. All are synchronous (block for the
// playback duration).
//
//   audio_beep        - SYS_BEEP=45 target: play freq_hz for duration_ms
//   audio_play_tone   - synonym for audio_beep (same implementation)
//   audio_play_pcm    - play interleaved 16-bit stereo PCM (one DMA buffer)
//   audio_startup_beep- bring up HDA if needed and emit a short boot chime
int audio_beep(uint32_t freq_hz, uint32_t duration_ms);
int audio_play_tone(uint32_t freq_hz, uint32_t ms);
int audio_play_pcm(const int16_t* buf, size_t frames, uint32_t rate);
int audio_startup_beep(void);

// Device management
audio_device_t* audio_device_alloc(const char* name, uint32_t type);
void audio_device_free(audio_device_t* dev);
int audio_device_register(audio_device_t* dev);
void audio_device_unregister(audio_device_t* dev);
audio_device_t* audio_device_get_default(uint32_t type);

// Audio operations
int audio_open(audio_device_t* dev);
int audio_close(audio_device_t* dev);
ssize_t audio_write(audio_device_t* dev, const void* data, size_t size);
ssize_t audio_read(audio_device_t* dev, void* data, size_t size);
int audio_ioctl(audio_device_t* dev, uint32_t cmd, void* arg);

// Format control
int audio_set_format(audio_device_t* dev, uint32_t format);
int audio_set_channels(audio_device_t* dev, uint32_t channels);
int audio_set_sample_rate(audio_device_t* dev, uint32_t rate);

// Playback control
int audio_start(audio_device_t* dev);
int audio_stop(audio_device_t* dev);
int audio_pause(audio_device_t* dev);
int audio_resume(audio_device_t* dev);
int audio_drain(audio_device_t* dev);
int audio_reset(audio_device_t* dev);

// Volume control
int audio_set_volume(audio_device_t* dev, uint32_t volume);
uint32_t audio_get_volume(audio_device_t* dev);
int audio_set_mute(audio_device_t* dev, bool mute);
bool audio_get_mute(audio_device_t* dev);

// Buffer management
int audio_get_buffer_info(audio_device_t* dev, audio_buf_info_t* info);
uint32_t audio_get_buffer_space(audio_device_t* dev);

// HDA backend integration
int audio_attach_hda(audio_device_t* dev, hda_controller_t* ctrl, hda_codec_t* codec);
void audio_detach_hda(audio_device_t* dev);

// /dev/dsp character device interface
int dsp_device_init(void);
void dsp_device_cleanup(void);

#endif // AUDIO_H
