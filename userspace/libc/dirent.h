// userspace/libc/dirent.h - Directory operations

#ifndef DIRENT_H
#define DIRENT_H

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned long size_t;   /* used by the DIR buffer fields below */
typedef unsigned long ino_t;
typedef long off_t;

// File types for d_type
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

// Maximum filename length
#define NAME_MAX 256

// Directory entry structure
struct dirent {
    ino_t d_ino;              // Inode number
    off_t d_off;              // Offset to next dirent
    unsigned short d_reclen;  // Length of this record
    unsigned char d_type;     // Type of file
    char d_name[NAME_MAX];    // Null-terminated filename
};

// Directory stream type (opaque)
typedef struct __dirstream DIR;

// Internal directory stream structure
struct __dirstream {
    int fd;                    // File descriptor
    char* buffer;              // Buffer for directory entries
    size_t buf_size;           // Buffer size
    size_t buf_pos;            // Current position in buffer
    size_t buf_count;          // Number of bytes in buffer
    struct dirent entry;       // Current directory entry
    long tell_pos;             // Current position for telldir/seekdir
};

// Directory operations
DIR* opendir(const char* name);
struct dirent* readdir(DIR* dirp);
int readdir_r(DIR* dirp, struct dirent* entry, struct dirent** result);
void rewinddir(DIR* dirp);
long telldir(DIR* dirp);
void seekdir(DIR* dirp, long loc);
int closedir(DIR* dirp);

// Directory file descriptor access
int dirfd(DIR* dirp);

// Scan directory
int scandir(const char* dirp, struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**));
int alphasort(const struct dirent** a, const struct dirent** b);

#endif
