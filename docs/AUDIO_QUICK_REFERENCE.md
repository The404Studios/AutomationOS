# Audio Subsystem - Quick Reference Card

## Userspace API

### Basic Playback
```c
#include <audio.h>

// 1. Open device
audio_t* audio = audio_open("/dev/dsp");

// 2. (Optional) Set format
audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE);

// 3. Play WAV file
audio_play_wav_file(audio, "file.wav");

// 4. Close
audio_close(audio);
```

### Writing PCM Data
```c
audio_t* audio = audio_open("/dev/dsp");
audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE);

int16_t samples[1024];
// ... fill samples ...

audio_write(audio, samples, sizeof(samples));
audio_drain(audio);
audio_close(audio);
```

### Volume Control
```c
audio_t* audio = audio_open("/dev/dsp");

// Set volume (0-100)
audio_set_volume(audio, 75);

// Get volume
int vol = audio_get_volume(audio);

// Mute/unmute
audio_set_mute(audio, true);
audio_set_mute(audio, false);

audio_close(audio);
```

## Kernel API

### Initialize Subsystem
```c
#include <audio.h>

// In kernel main
audio_init();
```

### Create Device
```c
// Allocate device
audio_device_t* dev = audio_device_alloc("dsp0", AUDIO_DEV_TYPE_PLAYBACK);

// Attach HDA hardware
audio_attach_hda(dev, hda_ctrl, codec);

// Register
audio_device_register(dev);
```

### Use Device
```c
// Open
audio_open(dev);

// Configure
audio_set_format(dev, AUDIO_FMT_S16_LE);
audio_set_channels(dev, 2);
audio_set_sample_rate(dev, 48000);

// Play
audio_start(dev);
audio_write(dev, data, size);
audio_stop(dev);

// Close
audio_close(dev);
```

## Command-Line Tools

### aplay
```bash
# Simple playback
aplay music.wav

# With volume
aplay -v 50 sound.wav

# Specific device
aplay -D /dev/dsp1 audio.wav

# Help
aplay --help
```

### audiomixerd
```bash
# Start daemon
audiomixerd

# Run in foreground
audiomixerd --foreground

# Help
audiomixerd --help
```

## Audio Formats

### Sample Rates
- `44100` - 44.1 kHz (CD quality)
- `48000` - 48 kHz (DVD quality)

### Formats
- `AUDIO_FORMAT_U8` - 8-bit unsigned
- `AUDIO_FORMAT_S16_LE` - 16-bit signed (most common)
- `AUDIO_FORMAT_S24_LE` - 24-bit signed
- `AUDIO_FORMAT_S32_LE` - 32-bit signed

### Channels
- `1` - Mono
- `2` - Stereo

## Common Tasks

### Play Startup Sound
```c
void play_startup_sound(void) {
    audio_t* audio = audio_open("/dev/dsp");
    if (audio) {
        audio_play_wav_file(audio, "/sounds/startup.wav");
        audio_close(audio);
    }
}
```

### Generate Tone
```c
#include <math.h>

void play_tone(float frequency, float duration) {
    audio_t* audio = audio_open("/dev/dsp");
    audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE);
    
    int samples = 48000 * duration;
    int16_t* buffer = malloc(samples * 4);  // stereo
    
    for (int i = 0; i < samples; i++) {
        float t = (float)i / 48000;
        int16_t sample = sinf(2 * M_PI * frequency * t) * 32767;
        buffer[i*2] = sample;      // left
        buffer[i*2+1] = sample;    // right
    }
    
    audio_write(audio, buffer, samples * 4);
    audio_drain(audio);
    
    free(buffer);
    audio_close(audio);
}
```

### WAV Header Parsing
```c
wav_info_t info;
const void* audio_data;

if (audio_wav_parse(wav_file, wav_size, &info, &audio_data) == 0) {
    printf("Sample rate: %u Hz\n", info.sample_rate);
    printf("Channels: %u\n", info.channels);
    printf("Bits: %u\n", info.bits_per_sample);
    printf("Data size: %u bytes\n", info.data_size);
}
```

## OSS IOCTLs

```c
#include <sys/ioctl.h>

int fd = open("/dev/dsp", O_RDWR);

// Reset device
ioctl(fd, SNDCTL_DSP_RESET, NULL);

// Set sample rate
int rate = 48000;
ioctl(fd, SNDCTL_DSP_SPEED, &rate);

// Set channels
int channels = 2;
ioctl(fd, SNDCTL_DSP_CHANNELS, &channels);

// Set format
int format = AUDIO_FORMAT_S16_LE;
ioctl(fd, SNDCTL_DSP_SETFMT, &format);

// Get buffer size
int blksize;
ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &blksize);

// Drain buffer
ioctl(fd, SNDCTL_DSP_SYNC, NULL);

close(fd);
```

## Error Handling

```c
audio_t* audio = audio_open("/dev/dsp");
if (!audio) {
    fprintf(stderr, "Error: %s\n", audio_get_error());
    return -1;
}

if (audio_play_wav_file(audio, "file.wav") != 0) {
    fprintf(stderr, "Playback error: %s\n", audio_get_error());
    audio_close(audio);
    return -1;
}

audio_close(audio);
```

## Compilation

### Userspace Programs
```makefile
# Makefile
CC = gcc
CFLAGS = -Wall -O2
LIBS = -laudio

myapp: myapp.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
```

### Manual Compilation
```bash
gcc -Wall -O2 -o myapp myapp.c -laudio
```

## Default Settings

| Setting | Default Value |
|---------|--------------|
| Sample Rate | 48000 Hz |
| Channels | 2 (stereo) |
| Format | 16-bit signed |
| Volume | 75% |
| Fragment Size | 4KB |
| Fragments | 4 |
| Buffer Size | 16KB |

## File Locations

| Component | Location |
|-----------|----------|
| Kernel driver | `kernel/drivers/audio/` |
| Kernel headers | `kernel/include/audio.h` |
| Userspace library | `userspace/lib/audio/` |
| Utilities | `userspace/bin/aplay`, `audiomixerd` |
| Documentation | `docs/AUDIO_*.md` |
| Tests | `tests/audio_test.c` |

## Debugging

### Enable Debug Output
```c
// In kernel code
#define AUDIO_DEBUG 1
```

### Check Logs
```bash
dmesg | grep -E "HDA|AUDIO"
```

### Test Device
```bash
# Test opening device
echo "test" > /dev/dsp

# Play test file
aplay test.wav
```

## Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| "No such device" | `/dev/dsp` doesn't exist | Initialize audio subsystem |
| "Device busy" | Already opened | Close other apps or reset device |
| "Invalid format" | Unsupported format | Use 44.1/48kHz, 16-bit |
| "Write failed" | Buffer full | Reduce write frequency |
| No sound | Volume is 0 or muted | Check volume settings |

## Performance Tips

1. **Reduce Latency**: Decrease fragment size and count
2. **Better Stability**: Increase fragment size and count
3. **Optimize CPU**: Use 16-bit format (native)
4. **Minimize Clicks**: Ensure continuous data flow

## Links

- **Full Documentation**: `docs/AUDIO_SUBSYSTEM.md`
- **Integration Guide**: `docs/AUDIO_INTEGRATION_GUIDE.md`
- **Kernel README**: `kernel/drivers/audio/README.md`
- **API Reference**: `userspace/lib/audio/audio.h`

---
**Quick Reference v1.0 - AutomationOS Audio Subsystem**
