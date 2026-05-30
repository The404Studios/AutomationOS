/**
 * Initial Ramdisk (initrd) Support
 */

#ifndef INITRD_H
#define INITRD_H

#include "types.h"

/**
 * Initialize initrd subsystem
 *
 * @param addr Physical address of initrd
 * @param size Size of initrd in bytes
 */
void initrd_init(uint64_t addr, uint64_t size);

/**
 * Mount initrd as root filesystem
 *
 * @return 0 on success, -1 on error
 */
int initrd_mount(void);

/**
 * Get file from initrd
 *
 * @param path File path
 * @param size_out Output: file size
 * @return Pointer to file data, or NULL if not found
 */
void* initrd_get_file(const char* path, uint64_t* size_out);

/**
 * List all files in initrd
 */
void initrd_list_files(void);

/**
 * Get initrd statistics
 *
 * @param total_files Output: total number of files
 * @param total_size Output: total size of all files
 */
void initrd_get_stats(uint64_t* total_files, uint64_t* total_size);

#endif // INITRD_H
