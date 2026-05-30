# Audio Subsystem Documentation

## Overview

The AutomationOS audio subsystem provides complete audio playback and recording support with an OSS-compatible `/dev/dsp` interface. The system is built on top of the Intel HDA driver and provides both kernel-level and userspace APIs.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Userspace Applications                    │
│  ┌──────────┐  ┌──────────┐  ┌─────────────┐  ┌──────────┐ │
│  │  aplay   │  │ audiomixerd│  │Media Player │  │  Games   │ │
│  └────┬─────┘  └─────┬──────┘  └──────┬──────┘  └────┬─────┘ │
└───────┼──────────────┼────────────────┼──────────────┼───────┘
        │              │                │              │
        └──────────────┴────────────────┴──────────────┘
                              │
        ┌─────────────────────▼───────────────────────┐
        │      Userspace Audio Library (libaudio)     │
        │  - audio_open(), audio_play_wav()           │
        │  - WAV file parsing                         │
        │  - OSS IOCTL wrappers                       │
        └─────────────────────┬───────────────────────┘
                              │
        ┌─────────────────────▼───────────────────────┐
        │           VFS Layer (/dev/dsp)              │
        │  - Character device interface               │
        │  - read/write/ioctl operations              │
        └─────────────────────┬───────────────────────┘
                              │
        ┌─────────────────────▼───────────────────────┐
        │       Audio Subsystem Core (audio_core.c)   │
        │  - Device abstraction                       │
        │  - Format conversion                        │
        │  - Buffer management                        │
        │  - Volume control                           │
        └─────────────────────┬───────────────────────┘
                              │
        ┌─────────────────────▼───────────────────────┐
        │         Intel HDA Driver (hda.c)            │
        │  - PCI enumeration                          │
        │  - Codec configuration                      │
        │  - DMA stream management                    │
        │  - Hardware control                         │
        └─────────────────────────────────────────────┘
```

## Components

### 1. Kernel Audio Subsystem

**Files:**
- `kernel/drivers/audio/audio_core.c` - Audio subsystem core
- `kernel/include/audio.h` - Audio API header

**Features:**
- Device abstraction layer over HDA hardware
- OSS-compatible IOCTL interface
- Format conversion and validation
- Volume control and muting
- Multiple device support

### 2. Intel HDA Driver

**Files:**
- `kernel/drivers/hda.c` - HDA controller driver
- `kernel/drivers/hda_stream.c` - Stream management
- `kernel/drivers/hda_wav.c` - WAV playback support
- `kernel/include/hda.h` - HDA definitions

**Features:**
- PCI device enumeration
- Codec initialization and configuration
- CORB/RIRB command/response buffers
- DMA buffer management
- Widget graph parsing
- Audio path setup (DAC → Pin)

### 3. Userspace Audio Library

**Files:**
- `userspace/lib/audio/audio.c` - Library implementation
- `userspace/lib/audio/audio.h` - Public API

**Features:**
- Simple C API for audio operations
- WAV file parsing and playback
- Format conversion
- Volume and mute control
- Error handling

### 4. Audio Utilities

**Files:**
- `userspace/bin/aplay.c` - WAV file player
- `userspace/bin/audiomixerd.c` - Software mixer daemon

**Features:**
- Command-line WAV playback
- Software mixing for multiple streams
- Volume control
- Progress indication

## API Documentation

### Userspace API

#### Opening Audio Device

```c
#include <audio.h>

audio_t* audio = audio_open("/dev/dsp");
if (!audio) {
    fprintf(stderr, "Failed to open audio: %s\n", audio_get_error());
    return -1;
}
```

#### Setting Format

```c
// Set to 48kHz, stereo, 16-bit
if (audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE) != 0) {
    fprintf(stderr, "Failed to set format\n");
}
```

#### Playing Audio

```c
// Write PCM data
int16_t samples[1024];
// ... fill samples ...
ssize_t written = audio_write(audio, samples, sizeof(samples));
```

#### Playing WAV Files

```c
// Play WAV file from memory
audio_play_wav(audio, wav_data, wav_size);

// Or play from disk
audio_play_wav_file(audio, "/path/to/file.wav");
```

#### Volume Control

```c
// Set volume (0-100)
audio_set_volume(audio, 75);

// Mute
audio_set_mute(audio, true);
```

#### Cleanup

```c
audio_drain(audio);  // Wait for playback to complete
audio_close(audio);
```

### Kernel API

#### Device Registration

```c
#include <audio.h>

// Initialize audio subsystem
audio_init();

// Create device
audio_device_t* dev = audio_device_alloc("dsp0", AUDIO_DEV_TYPE_PLAYBACK);

// Attach HDA hardware
audio_attach_hda(dev, hda_ctrl, codec);

// Register device
audio_device_register(dev);
```

#### Audio Operations

```c
// Open device
audio_open(dev);

// Configure format
audio_set_format(dev, AUDIO_FMT_S16_LE);
audio_set_channels(dev, 2);
audio_set_sample_rate(dev, 48000);

// Start playback
audio_start(dev);

// Write data
audio_write(dev, pcm_data, size);

// Stop playback
audio_stop(dev);

// Close device
audio_close(dev);
```

## Supported Formats

### Sample Rates
- 44.1 kHz (CD quality)
- 48 kHz (DVD quality)

### Bit Depths
- 8-bit unsigned
- 16-bit signed (most common)
- 24-bit signed
- 32-bit signed

### Channels
- Mono (1 channel)
- Stereo (2 channels)

## WAV File Format

The audio library supports standard PCM WAV files:

```
WAV File Structure:
┌──────────────────────┐
│   RIFF Header        │ - "RIFF" magic
│   File Size          │ - Total file size - 8
│   "WAVE" Format      │ - Format identifier
├──────────────────────┤
│   fmt Chunk          │ - Format details
│   - Audio Format (1) │ - 1 = PCM
│   - Channels         │ - 1 or 2
│   - Sample Rate      │ - Hz
│   - Byte Rate        │ - Bytes per second
│   - Block Align      │ - Bytes per sample
│   - Bits/Sample      │ - 8, 16, 24, 32
├──────────────────────┤
│   data Chunk         │ - Audio data
│   - Size             │ - Data size in bytes
│   - PCM Samples      │ - Raw audio data
└──────────────────────┘
```

## Usage Examples

### Playing a WAV File

```bash
# Simple playback
aplay music.wav

# With volume control
aplay -v 50 sound.wav

# Specific device
aplay -D /dev/dsp1 audio.wav
```

### Programming Example

```c
#include <audio.h>

int main(void) {
    // Open audio device
    audio_t* audio = audio_open("/dev/dsp");
    if (!audio) {
        fprintf(stderr, "Failed to open audio\n");
        return 1;
    }

    // Set format: 48kHz, stereo, 16-bit
    audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE);

    // Set volume
    audio_set_volume(audio, 75);

    // Play WAV file
    if (audio_play_wav_file(audio, "music.wav") != 0) {
        fprintf(stderr, "Playback failed: %s\n", audio_get_error());
        audio_close(audio);
        return 1;
    }

    // Cleanup
    audio_close(audio);
    return 0;
}
```

### Generating Audio

```c
#include <audio.h>
#include <math.h>

// Generate a 440Hz sine wave (A4 note)
void generate_tone(audio_t* audio, float duration) {
    const uint32_t sample_rate = 48000;
    const uint32_t samples = sample_rate * duration;
    int16_t* buffer = malloc(samples * sizeof(int16_t) * 2);  // Stereo

    for (uint32_t i = 0; i < samples; i++) {
        float t = (float)i / sample_rate;
        float value = sinf(2.0f * M_PI * 440.0f * t);
        int16_t sample = (int16_t)(value * 32767.0f);

        buffer[i * 2] = sample;      // Left
        buffer[i * 2 + 1] = sample;  // Right
    }

    audio_write(audio, buffer, samples * sizeof(int16_t) * 2);
    free(buffer);
}
```

## Audio Mixer

The `audiomixerd` daemon provides software mixing for multiple audio streams:

```bash
# Start mixer daemon
audiomixerd

# Run in foreground
audiomixerd --foreground
```

Features:
- Mix up to 8 simultaneous audio streams
- Per-client volume control
- Automatic client connection handling
- Software clipping prevention

## OSS Compatibility

The `/dev/dsp` interface is compatible with OSS (Open Sound System):

### IOCTLs

- `SNDCTL_DSP_RESET` - Reset device
- `SNDCTL_DSP_SYNC` - Wait for playback to complete
- `SNDCTL_DSP_SPEED` - Set sample rate
- `SNDCTL_DSP_STEREO` - Set stereo mode
- `SNDCTL_DSP_CHANNELS` - Set number of channels
- `SNDCTL_DSP_SETFMT` - Set audio format
- `SNDCTL_DSP_GETBLKSIZE` - Get buffer block size
- `SNDCTL_DSP_GETCAPS` - Get device capabilities

## Performance Considerations

### Buffer Sizes

Default buffer configuration:
- Fragment size: 4KB
- Number of fragments: 4
- Total buffer: 16KB

At 48kHz stereo 16-bit:
- Bytes per second: 192,000
- Buffer duration: ~83ms
- Latency: ~21ms per fragment

### DMA Buffers

The HDA driver uses:
- 64KB circular DMA buffer
- 8 BDL entries of 8KB each
- Interrupt on completion for each entry

## Testing

### Basic Playback Test

```c
// kernel/drivers/hda_test.c
void hda_test_playback(void) {
    // Generates 440Hz tone
    hda_run_tests();
}
```

### WAV Playback Test

```c
// kernel/drivers/hda_wav.c
void hda_test_wav_playback(void) {
    // Generates and plays test WAV
}
```

### User-space Test

```bash
# Test audio device
echo "Testing audio" > /dev/dsp

# Play test tone
aplay test.wav

# Check device info
cat /proc/asound/devices
```

## Troubleshooting

### No Audio Output

1. Check HDA initialization:
   ```
   dmesg | grep HDA
   ```

2. Verify codec configuration:
   ```
   cat /proc/asound/codec#0
   ```

3. Check volume:
   ```
   amixer set Master 75%
   ```

### Distorted Audio

- Check sample rate matches source
- Verify buffer sizes are adequate
- Check for clipping in mixer

### Device Busy

- Ensure no other process has device open
- Stop audiomixerd if running
- Reset device: `echo "reset" > /dev/dsp`

## Future Enhancements

1. **ALSA Compatibility** - Add ALSA API layer
2. **Recording Support** - Implement ADC path
3. **Hardware Mixing** - Use HDA mixer widgets
4. **AC97 Support** - Add legacy audio support
5. **PulseAudio** - Port PulseAudio daemon
6. **JACK** - Low-latency audio support
7. **Bluetooth Audio** - A2DP/HSP profiles
8. **USB Audio** - USB Audio Class support

## References

- Intel High Definition Audio Specification
- OSS API Documentation
- ALSA Project Documentation
- WAV File Format Specification
