/*
 * ide_config.h -- persist the IDE Settings knobs (ide_gfx.c runtime vars) as a
 * simple key=value text file, so the user's preferences survive across IDE
 * restarts (and, once a durable /persist mount exists, across reboots).
 *
 * The path is centralized in IDE_CONFIG_PATH (one #define). ide_config_load()
 * tries the durable path first, then a session-writable fallback; values are
 * clamped on load (corrupt/partial -> defaults). ide_config_save() is called
 * after every Settings change.
 *
 * Freestanding: no libc. IO via ide_read_file / ide_write_file (ide_sys.c).
 */
#ifndef IDE_CONFIG_H
#define IDE_CONFIG_H

/* Primary store is the kernel's durable diskfs (SYS_PERSIST_READ/WRITE), a flat
 * named file that SURVIVES A REBOOT on a present SATA disk. With no disk those
 * syscalls return an error and we fall back to a session-local file (RAM-backed
 * but a real, working file -- the IDE already writes build output under /Desktop).
 * So: reboot-durable when a disk is present; session-durable otherwise. */
#define IDE_PERSIST_NAME     "ide.config"        /* diskfs flat file name */
#define IDE_CONFIG_FALLBACK  "/Desktop/.ide_config"

/* Load persisted knob values into the ide_gfx.c runtime vars (clamped). Applies
 * the font scale via gfx_set_scale. Safe + silent if no config file exists. */
void ide_config_load(void);

/* Persist the current knob values. Tries IDE_CONFIG_PATH, then the fallback. */
void ide_config_save(void);

#endif /* IDE_CONFIG_H */
