// userspace/libc/stdio.h - Standard I/O declarations

#ifndef STDIO_H
#define STDIO_H

#ifndef NULL
#define NULL ((void*)0)
#endif
typedef unsigned long size_t;
typedef long ssize_t;

// File position type
typedef long fpos_t;

// End-of-file indicator
#define EOF (-1)

// File buffering modes
#define _IOFBF 0  // Fully buffered
#define _IOLBF 1  // Line buffered
#define _IONBF 2  // Unbuffered

// Buffer size
#define BUFSIZ 8192

// Seek positions
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// File stream structure
typedef struct {
    int fd;              // File descriptor
    int flags;           // File flags (read/write/error/eof)
    int error;           // Error indicator
    int eof;             // EOF indicator
    char* buffer;        // I/O buffer
    size_t buf_size;     // Buffer size
    size_t buf_pos;      // Current position in buffer
    size_t buf_count;    // Number of bytes in buffer
    int buf_mode;        // Buffering mode
} FILE;

// File flags
#define _FILE_READ  0x01
#define _FILE_WRITE 0x02
#define _FILE_EOF   0x04
#define _FILE_ERROR 0x08

// Standard streams
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// Standard I/O functions
int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* buffer, const char* format, ...);
int snprintf(char* buffer, size_t size, const char* format, ...);
int vprintf(const char* format, __builtin_va_list args);
int vfprintf(FILE* stream, const char* format, __builtin_va_list args);
int vsprintf(char* buffer, const char* format, __builtin_va_list args);
int vsnprintf(char* buffer, size_t size, const char* format, __builtin_va_list args);
int puts(const char* str);
int putchar(int c);
int getchar(void);
int fileno(FILE* stream);

// File operations
FILE* fopen(const char* filename, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);
int fgetpos(FILE* stream, fpos_t* pos);
int fsetpos(FILE* stream, const fpos_t* pos);

// Character I/O
int fgetc(FILE* stream);
int fputc(int c, FILE* stream);
char* fgets(char* s, int size, FILE* stream);
int fputs(const char* s, FILE* stream);
int ungetc(int c, FILE* stream);

// Formatted I/O
int fscanf(FILE* stream, const char* format, ...);
int scanf(const char* format, ...);
int sscanf(const char* str, const char* format, ...);

// Error handling
void clearerr(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void perror(const char* s);

// Buffering
int setvbuf(FILE* stream, char* buf, int mode, size_t size);
void setbuf(FILE* stream, char* buf);
int fflush(FILE* stream);

#endif
