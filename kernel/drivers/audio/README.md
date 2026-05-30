# Audio Subsystem

Audio support for AutomationOS with OSS-compatible `/dev/dsp` interface.

## Features

- Intel HDA hardware support
- OSS-compatible character device interface
- WAV file playback
- Volume control and muting
- Multiple audio devices
- Software mixing support

## Architecture

The audio subsystem consists of three layers:

1. **Hardware Layer** - Intel HDA driver (../hda*.c)
2. **Abstraction Layer** - Audio core (audio_core.c)
3. **Interface Layer** - /dev/dsp character device

## Building

```bash
cd kernel/drivers/audio
make
```

This produces `audio.a` which is linked into the kernel.

## Integration

To integrate the audio subsystem into the kernel:

### 1. Add to Kernel Makefile

```makefile
# In kernel/Makefile
AUDIO_OBJS = drivers/audio/audio.a

kernel.bin: ... $(AUDIO_OBJS)
	$(LD) ... $(AUDIO_OBJS) ...
```

### 2. Initialize in Kernel

```c
// In kernel/init/main.c
#include <audio.h>

void kernel_main(void) {
    // ... other initialization ...
    
    // Initialize HDA driver
    hda_init();
    
    // Initialize audio subsystem
    audio_init();
    
    // ... continue initialization ...
}
```

### 3. Create Device Node

```c
// In VFS initialization
vfs_create_device("/dev/dsp", DEVICE_AUDIO);
```

## API

### Kernel API

```c
#include <audio.h>

// Initialize subsystem
audio_init();

// Get default playback device
audio_device_t* dev = audio_device_get_default(AUDIO_DEV_TYPE_PLAYBACK);

// Configure and play
audio_open(dev);
audio_set_format(dev, AUDIO_FMT_S16_LE);
audio_set_sample_rate(dev, 48000);
audio_set_channels(dev, 2);
audio_start(dev);
audio_write(dev, pcm_data, size);
audio_drain(dev);
audio_close(dev);
```

### Userspace API

See `userspace/lib/audio/audio.h` for userspace API.

## Testing

### Kernel Tests

```c
// Run HDA tests
hda_run_tests();

// Test WAV playback
hda_test_wav_playback();
```

### Userspace Tests

```bash
# Play WAV file
aplay test.wav

# Start mixer daemon
audiomixerd --foreground
```

## Device Nodes

- `/dev/dsp` - Primary audio device (playback/recording)
- `/dev/dsp1` - Secondary audio device (if available)
- `/dev/mixer` - Mixer control (future)

## IOCTLs

Supported OSS IOCTLs:

| IOCTL | Description |
|-------|-------------|
| SNDCTL_DSP_RESET | Reset device |
| SNDCTL_DSP_SYNC | Drain buffer |
| SNDCTL_DSP_SPEED | Set sample rate |
| SNDCTL_DSP_STEREO | Set stereo mode |
| SNDCTL_DSP_CHANNELS | Set channels |
| SNDCTL_DSP_SETFMT | Set format |
| SNDCTL_DSP_GETBLKSIZE | Get block size |
| SNDCTL_DSP_GETCAPS | Get capabilities |

## Supported Formats

- **Sample Rates**: 44.1kHz, 48kHz
- **Bit Depths**: 8, 16, 24, 32 bits
- **Channels**: Mono, Stereo

## Configuration

Default configuration in `audio_core.c`:

```c
#define DEFAULT_SAMPLE_RATE    48000
#define DEFAULT_CHANNELS       2
#define DEFAULT_FORMAT         AUDIO_FMT_S16_LE
#define DEFAULT_FRAGMENT_SIZE  4096
#define DEFAULT_FRAGMENTS      4
```

## Debugging

Enable debug output:

```c
// In audio_core.c
#define AUDIO_DEBUG 1
```

Check initialization:

```bash
dmesg | grep AUDIO
```

Expected output:
```
AUDIO: Initializing audio subsystem
AUDIO: Registered device 'dsp0'
AUDIO: /dev/dsp initialized
AUDIO: Initialization complete
```

## Performance

Buffer configuration:
- Fragment size: 4KB
- Fragments: 4
- Total buffer: 16KB
- Latency: ~21ms @ 48kHz stereo 16-bit

## Troubleshooting

### No audio output

1. Check HDA initialization:
   ```
   dmesg | grep HDA
   ```

2. Verify codec is configured:
   ```
   HDA: Found codec at address X
   HDA: Found DAC at NID Y
   HDA: Found output pin at NID Z
   ```

3. Check volume is not zero:
   ```c
   audio_set_volume(dev, 75);
   ```

### Device busy

- Close all applications using audio
- Reset device:
  ```c
  audio_reset(dev);
  ```

### Distorted audio

- Check sample rate matches source
- Verify format is correct
- Check for buffer underruns

## See Also

- `docs/AUDIO_SUBSYSTEM.md` - Complete documentation
- `userspace/lib/audio/` - Userspace library
- `userspace/bin/aplay.c` - Audio player utility
- `kernel/drivers/hda.c` - HDA hardware driver
