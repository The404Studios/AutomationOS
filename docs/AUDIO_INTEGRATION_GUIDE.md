# Audio Subsystem Integration Guide

This guide walks through integrating the audio subsystem into AutomationOS.

## Prerequisites

- Intel HDA driver initialized (`hda_init()`)
- VFS layer operational
- Memory management (kmalloc/kfree)
- PCI enumeration working

## Step 1: Kernel Integration

### 1.1 Update Kernel Makefile

Add audio subsystem to the build:

```makefile
# In kernel/Makefile

# Add audio driver directory
DRIVER_DIRS = drivers/core drivers/input drivers/storage drivers/audio

# Include audio objects
AUDIO_OBJS = drivers/audio/audio.a drivers/hda.o drivers/hda_stream.o drivers/hda_wav.o

# Link audio objects
kernel.bin: $(KERNEL_OBJS) $(AUDIO_OBJS)
	$(LD) -n -o $@ -T linker.ld $(KERNEL_OBJS) $(AUDIO_OBJS)
```

### 1.2 Initialize in Kernel Main

```c
// In kernel/init/main.c

#include <hda.h>
#include <audio.h>

void kernel_main(multiboot_info_t* mboot_info) {
    // ... early initialization ...
    
    serial_write("Initializing PCI\n", 17);
    pci_init();
    
    serial_write("Initializing HDA\n", 17);
    hda_init();
    
    serial_write("Initializing audio subsystem\n", 30);
    audio_init();
    
    // ... continue initialization ...
}
```

### 1.3 Create Device Node

Add to VFS initialization:

```c
// In kernel/fs/vfs.c or appropriate initialization

#include <audio.h>

void vfs_create_dev_nodes(void) {
    // Create /dev directory
    vfs_mkdir(NULL, "/dev", 0755);
    
    // Create /dev/dsp device node
    vfs_inode_t* dsp_inode = vfs_create_inode(VFS_TYPE_DEVICE);
    dsp_inode->mode = 0666;  // rw-rw-rw-
    
    // Set up device operations
    extern vfs_file_ops_t dsp_fops;
    dsp_inode->ops->file_ops = &dsp_fops;
    
    // Add to /dev
    vfs_link(vfs_root, "dev/dsp", dsp_inode);
}
```

## Step 2: Userspace Library

### 2.1 Build Library

```bash
cd userspace/lib/audio
make
make install
```

This creates:
- `build/lib/libaudio.a` - Static library
- `build/include/audio.h` - Public header

### 2.2 Link Against Library

When building userspace applications:

```makefile
# In userspace/bin/Makefile

LIBS = -L../build/lib -laudio
INCLUDES = -I../build/include

aplay: aplay.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)
```

## Step 3: Build Audio Utilities

### 3.1 Build aplay

```bash
cd userspace/bin
make aplay
```

### 3.2 Build audiomixerd

```bash
cd userspace/bin
make audiomixerd
```

## Step 4: Testing

### 4.1 Kernel Tests

```c
// In kernel test harness or boot sequence

void test_audio(void) {
    serial_write("Testing HDA driver\n", 19);
    hda_run_tests();
    
    serial_write("Testing WAV playback\n", 21);
    hda_test_wav_playback();
}
```

### 4.2 Userspace Tests

```bash
# Test basic playback
aplay test.wav

# Test with options
aplay -v 50 -D /dev/dsp test.wav

# Start mixer daemon
audiomixerd --foreground
```

## Step 5: Application Integration

### 5.1 Simple Playback

```c
#include <audio.h>

int play_sound(const char* filename) {
    // Open audio device
    audio_t* audio = audio_open("/dev/dsp");
    if (!audio) {
        return -1;
    }
    
    // Play WAV file
    int result = audio_play_wav_file(audio, filename);
    
    // Cleanup
    audio_close(audio);
    return result;
}
```

### 5.2 Streaming Audio

```c
#include <audio.h>

void stream_audio(void) {
    audio_t* audio = audio_open("/dev/dsp");
    
    // Configure format
    audio_set_format(audio, 48000, 2, AUDIO_FORMAT_S16_LE);
    
    // Stream audio in chunks
    int16_t buffer[4096];
    while (generate_audio(buffer, 4096)) {
        audio_write(audio, buffer, sizeof(buffer));
    }
    
    audio_drain(audio);
    audio_close(audio);
}
```

### 5.3 Volume Control

```c
#include <audio.h>

void adjust_volume(int change) {
    audio_t* audio = audio_open("/dev/dsp");
    
    int current = audio_get_volume(audio);
    int new_volume = current + change;
    
    if (new_volume < 0) new_volume = 0;
    if (new_volume > 100) new_volume = 100;
    
    audio_set_volume(audio, new_volume);
    audio_close(audio);
}
```

## Complete Example: Media Player

```c
#include <stdio.h>
#include <stdlib.h>
#include <audio.h>

typedef struct {
    audio_t* audio;
    bool playing;
    bool paused;
    int volume;
} media_player_t;

media_player_t* player_create(void) {
    media_player_t* player = malloc(sizeof(media_player_t));
    if (!player) return NULL;
    
    player->audio = audio_open("/dev/dsp");
    if (!player->audio) {
        free(player);
        return NULL;
    }
    
    player->playing = false;
    player->paused = false;
    player->volume = 75;
    
    audio_set_volume(player->audio, player->volume);
    
    return player;
}

void player_destroy(media_player_t* player) {
    if (player) {
        if (player->audio) {
            audio_close(player->audio);
        }
        free(player);
    }
}

int player_play(media_player_t* player, const char* filename) {
    if (!player || !filename) return -1;
    
    player->playing = true;
    player->paused = false;
    
    int result = audio_play_wav_file(player->audio, filename);
    
    player->playing = false;
    return result;
}

void player_set_volume(media_player_t* player, int volume) {
    if (!player) return;
    
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    
    player->volume = volume;
    audio_set_volume(player->audio, volume);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.wav>\n", argv[0]);
        return 1;
    }
    
    media_player_t* player = player_create();
    if (!player) {
        fprintf(stderr, "Failed to create player\n");
        return 1;
    }
    
    printf("Playing: %s\n", argv[1]);
    if (player_play(player, argv[1]) != 0) {
        fprintf(stderr, "Playback failed\n");
        player_destroy(player);
        return 1;
    }
    
    player_destroy(player);
    return 0;
}
```

## Bootloader Integration

### Include in Initial Ramdisk

```bash
# In scripts/mkinitrd.sh

# Add audio utilities
cp userspace/bin/aplay initrd/bin/
cp userspace/bin/audiomixerd initrd/sbin/

# Add audio library
mkdir -p initrd/lib
cp userspace/build/lib/libaudio.a initrd/lib/

# Add test sounds
mkdir -p initrd/sounds
cp test_sounds/*.wav initrd/sounds/
```

### Auto-start Audio Mixer

```bash
# In init script or rc.local

# Start audio mixer daemon
/sbin/audiomixerd &

# Play startup sound
/bin/aplay /sounds/startup.wav &
```

## Debugging

### Enable Debug Output

```c
// In kernel/drivers/audio/audio_core.c
#define AUDIO_DEBUG 1

// In kernel/drivers/hda.c
#define HDA_DEBUG 1
```

### Check Initialization

```bash
# Kernel messages
dmesg | grep -E "HDA|AUDIO"

# Expected output:
# HDA: Found HD Audio controller
# HDA: MMIO base at 0x...
# HDA: Controller reset complete
# HDA: CORB/RIRB initialized
# HDA: Found codec at address 0
# HDA: Vendor ID = 0x...
# HDA: Found AFG at NID ...
# HDA: Found DAC at NID ...
# HDA: Found output pin at NID ...
# HDA: Output path configured
# HDA: Found 1 codec(s)
# HDA: Initialization complete
# AUDIO: Initializing audio subsystem
# AUDIO: Attached HDA backend to 'dsp0'
# AUDIO: Registered device 'dsp0'
# AUDIO: Registered default playback device
# AUDIO: /dev/dsp initialized
# AUDIO: Initialization complete
```

### Test Device Access

```c
// Test opening device
int fd = open("/dev/dsp", O_RDWR);
if (fd < 0) {
    perror("Failed to open /dev/dsp");
}
close(fd);
```

### Monitor Audio Activity

```bash
# Watch for audio writes
watch -n 1 'cat /proc/audio/stats'

# Monitor buffer usage
cat /proc/audio/buffers
```

## Performance Tuning

### Adjust Buffer Sizes

```c
// In kernel/drivers/audio/audio_core.c

// Reduce latency (smaller buffers)
#define DEFAULT_FRAGMENT_SIZE  2048  // 2KB instead of 4KB
#define DEFAULT_FRAGMENTS      2     // 2 instead of 4

// Or increase buffer for better stability
#define DEFAULT_FRAGMENT_SIZE  8192  // 8KB
#define DEFAULT_FRAGMENTS      8     // 8 fragments
```

### DMA Buffer Configuration

```c
// In kernel/drivers/hda_stream.c

// Larger DMA buffer for better stability
#define HDA_DMA_BUFFER_SIZE  (32 * 4096)  // 128KB instead of 64KB
```

## Troubleshooting

### No Sound

1. Check codec initialization:
   ```bash
   dmesg | grep "HDA: Found codec"
   ```

2. Verify output path:
   ```bash
   dmesg | grep "Output path configured"
   ```

3. Check volume:
   ```c
   audio_set_volume(audio, 100);
   ```

### Distorted Sound

1. Verify sample rate matches:
   ```c
   audio_set_format(audio, wav_info.sample_rate, ...);
   ```

2. Check for clipping:
   - Reduce volume
   - Check mixer settings

### Device Busy

1. Check for open handles:
   ```bash
   lsof /dev/dsp
   ```

2. Reset device:
   ```c
   audio_reset(dev);
   ```

## Next Steps

1. **Add ALSA Compatibility** - Implement ALSA API layer
2. **Recording Support** - Enable ADC path
3. **Multiple Applications** - Implement proper mixing
4. **PulseAudio Port** - Port PulseAudio daemon
5. **USB Audio** - Add USB Audio Class support

## References

- See `docs/AUDIO_SUBSYSTEM.md` for architecture details
- See `kernel/drivers/audio/README.md` for API reference
- See Intel HDA specification for hardware details
