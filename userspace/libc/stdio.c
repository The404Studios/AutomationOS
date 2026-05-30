// userspace/libc/stdio.c - Standard I/O functions

#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"

// Buffers for standard streams
static char stdout_buffer[BUFSIZ];
static char stdin_buffer[BUFSIZ];

// Standard streams
static FILE stdin_file = { STDIN_FILENO, _FILE_READ, 0, 0, stdin_buffer, BUFSIZ, 0, 0, _IOFBF };
static FILE stdout_file = { STDOUT_FILENO, _FILE_WRITE, 0, 0, stdout_buffer, BUFSIZ, 0, 0, _IOLBF };
static FILE stderr_file = { STDERR_FILENO, _FILE_WRITE, 0, 0, NULL, 0, 0, 0, _IONBF };

FILE* stdin = &stdin_file;
FILE* stdout = &stdout_file;
FILE* stderr = &stderr_file;

// ============================================================================
// FORMATTED OUTPUT CORE
// ============================================================================
//
// A single formatter, format_core(), writes into a "sink" abstraction so the
// same code backs snprintf (write to a bounded buffer) and printf/fprintf
// (write to a file descriptor). It supports flags (- + space # 0), field
// width and precision (both via number or '*'), length modifiers (hh h l ll z)
// and conversions d i u o x X c s p %. Floating point (%f/%g/%e) is not
// supported (no libm dependency in stdio); use the math/dtoa path if needed.

typedef struct {
    char* buffer;        // bounded buffer (snprintf), or NULL for fd sink
    size_t size;         // buffer capacity incl. NUL
    int fd;              // fd for the fd sink
    size_t count;        // total characters that *would* have been written
    char fdbuf[256];     // staging buffer for the fd sink
    size_t fdlen;        // bytes pending in fdbuf
} sink_t;

static void sink_flush(sink_t* s) {
    if (s->fdlen) {
        write(s->fd, s->fdbuf, s->fdlen);
        s->fdlen = 0;
    }
}

static void sink_putc(sink_t* s, char c) {
    if (s->buffer) {
        if (s->size > 0 && s->count < s->size - 1) {
            s->buffer[s->count] = c;
        }
    } else {
        if (s->fdlen >= sizeof(s->fdbuf)) {
            sink_flush(s);
        }
        s->fdbuf[s->fdlen++] = c;
    }
    s->count++;
}

// Format flags.
#define FL_LEFT   0x01
#define FL_PLUS   0x02
#define FL_SPACE  0x04
#define FL_HASH   0x08
#define FL_ZERO   0x10

static int read_int(const char** pp) {
    int v = 0;
    while (**pp >= '0' && **pp <= '9') {
        v = v * 10 + (**pp - '0');
        (*pp)++;
    }
    return v;
}

// Emit a numeric magnitude (already converted to digits in tmp[len..]) with
// the requested padding, sign/prefix and precision handling.
static void emit_number(sink_t* s, const char* digits, int len,
                        const char* prefix, char sign,
                        int flags, int width, int precision) {
    int prefix_len = 0;
    while (prefix && prefix[prefix_len]) {
        prefix_len++;
    }
    int sign_len = sign ? 1 : 0;

    int zeros = 0;
    if (precision >= 0 && precision > len) {
        zeros = precision - len;
    }

    int body = sign_len + prefix_len + zeros + len;
    int pad = (width > body) ? width - body : 0;

    // With a precision, the '0' flag is ignored.
    int zero_pad = (flags & FL_ZERO) && !(flags & FL_LEFT) && precision < 0;

    if (!(flags & FL_LEFT) && !zero_pad) {
        while (pad-- > 0) sink_putc(s, ' ');
    }
    if (sign) sink_putc(s, sign);
    for (int i = 0; i < prefix_len; i++) sink_putc(s, prefix[i]);
    if (zero_pad) {
        while (pad-- > 0) sink_putc(s, '0');
    }
    while (zeros-- > 0) sink_putc(s, '0');
    for (int i = 0; i < len; i++) sink_putc(s, digits[i]);
    if (flags & FL_LEFT) {
        while (pad-- > 0) sink_putc(s, ' ');
    }
}

static void emit_string(sink_t* s, const char* str, int flags,
                        int width, int precision) {
    if (!str) {
        str = "(null)";
    }
    int len = 0;
    while (str[len] && (precision < 0 || len < precision)) {
        len++;
    }
    int pad = (width > len) ? width - len : 0;
    if (!(flags & FL_LEFT)) {
        while (pad-- > 0) sink_putc(s, ' ');
    }
    for (int i = 0; i < len; i++) sink_putc(s, str[i]);
    if (flags & FL_LEFT) {
        while (pad-- > 0) sink_putc(s, ' ');
    }
}

static int format_core(sink_t* s, const char* format, __builtin_va_list args) {
    for (const char* p = format; *p; p++) {
        if (*p != '%') {
            sink_putc(s, *p);
            continue;
        }

        p++;  // consume '%'

        // Flags.
        int flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= FL_LEFT;
            else if (*p == '+') flags |= FL_PLUS;
            else if (*p == ' ') flags |= FL_SPACE;
            else if (*p == '#') flags |= FL_HASH;
            else if (*p == '0') flags |= FL_ZERO;
            else break;
        }

        // Width.
        int width = 0;
        if (*p == '*') {
            width = __builtin_va_arg(args, int);
            if (width < 0) { flags |= FL_LEFT; width = -width; }
            p++;
        } else {
            width = read_int(&p);
        }

        // Precision.
        int precision = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                precision = __builtin_va_arg(args, int);
                p++;
            } else {
                precision = read_int(&p);
            }
            if (precision < 0) precision = -1;
        }

        // Length modifiers.
        int length = 0;  // 0=int, -1=short, -2=char, 1=long, 2=long long, 3=size_t
        if (*p == 'h') {
            p++;
            if (*p == 'h') { length = -2; p++; }
            else           { length = -1; }
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') { length = 2; p++; }
            else           { length = 1; }
        } else if (*p == 'z') {
            length = 3; p++;
        }

        if (*p == '\0') {
            break;  // malformed trailing specifier
        }

        char conv = *p;
        char tmp[32];

        switch (conv) {
            case 'd':
            case 'i': {
                long long v;
                switch (length) {
                    case 2:  v = __builtin_va_arg(args, long long); break;
                    case 1:  v = __builtin_va_arg(args, long); break;
                    case 3:  v = (long long)__builtin_va_arg(args, long); break;
                    case -1: v = (short)__builtin_va_arg(args, int); break;
                    case -2: v = (signed char)__builtin_va_arg(args, int); break;
                    default: v = __builtin_va_arg(args, int); break;
                }
                char sign = 0;
                unsigned long long mag;
                if (v < 0) { sign = '-'; mag = (unsigned long long)(-v); }
                else {
                    mag = (unsigned long long)v;
                    if (flags & FL_PLUS)  sign = '+';
                    else if (flags & FL_SPACE) sign = ' ';
                }
                int n = 0;
                if (mag == 0 && precision != 0) tmp[n++] = '0';
                while (mag > 0) { tmp[n++] = (char)('0' + mag % 10); mag /= 10; }
                // reverse
                for (int a = 0, b = n - 1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
                emit_number(s, tmp, n, NULL, sign, flags, width, precision);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                unsigned long long v;
                switch (length) {
                    case 2:  v = __builtin_va_arg(args, unsigned long long); break;
                    case 1:  v = __builtin_va_arg(args, unsigned long); break;
                    case 3:  v = __builtin_va_arg(args, unsigned long); break;
                    case -1: v = (unsigned short)__builtin_va_arg(args, unsigned int); break;
                    case -2: v = (unsigned char)__builtin_va_arg(args, unsigned int); break;
                    default: v = __builtin_va_arg(args, unsigned int); break;
                }
                int base = (conv == 'o') ? 8 : (conv == 'u') ? 10 : 16;
                const char* digs = (conv == 'X') ? "0123456789ABCDEF"
                                                 : "0123456789abcdef";
                int n = 0;
                if (v == 0 && precision != 0) tmp[n++] = '0';
                while (v > 0) { tmp[n++] = digs[v % (unsigned)base]; v /= (unsigned)base; }
                for (int a = 0, b = n - 1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
                const char* prefix = NULL;
                if ((flags & FL_HASH) && n > 0) {
                    if (conv == 'x') prefix = "0x";
                    else if (conv == 'X') prefix = "0X";
                    else if (conv == 'o' && tmp[0] != '0') prefix = "0";
                }
                emit_number(s, tmp, n, prefix, 0, flags, width, precision);
                break;
            }
            case 'p': {
                unsigned long long v = (unsigned long long)(unsigned long)__builtin_va_arg(args, void*);
                const char* digs = "0123456789abcdef";
                int n = 0;
                if (v == 0) tmp[n++] = '0';
                while (v > 0) { tmp[n++] = digs[v & 0xf]; v >>= 4; }
                for (int a = 0, b = n - 1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
                emit_number(s, tmp, n, "0x", 0, flags, width, -1);
                break;
            }
            case 'c': {
                char c = (char)__builtin_va_arg(args, int);
                int pad = (width > 1) ? width - 1 : 0;
                if (!(flags & FL_LEFT)) while (pad-- > 0) sink_putc(s, ' ');
                sink_putc(s, c);
                if (flags & FL_LEFT) while (pad-- > 0) sink_putc(s, ' ');
                break;
            }
            case 's':
                emit_string(s, __builtin_va_arg(args, const char*), flags, width, precision);
                break;
            case '%':
                sink_putc(s, '%');
                break;
            default:
                sink_putc(s, '%');
                sink_putc(s, conv);
                break;
        }
    }

    return (int)s->count;
}

int vsnprintf(char* buffer, size_t size, const char* format, __builtin_va_list args) {
    sink_t s = { buffer, size, -1, 0, {0}, 0 };
    int n = format_core(&s, format, args);
    if (buffer && size > 0) {
        size_t idx = (s.count < size) ? s.count : size - 1;
        buffer[idx] = '\0';
    }
    return n;
}

int vsprintf(char* buffer, const char* format, __builtin_va_list args) {
    return vsnprintf(buffer, (size_t)-1, format, args);
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    __builtin_va_end(args);
    return result;
}

int sprintf(char* buffer, const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int result = vsnprintf(buffer, (size_t)-1, format, args);
    __builtin_va_end(args);
    return result;
}

int vfprintf(FILE* stream, const char* format, __builtin_va_list args) {
    sink_t s = { NULL, 0, stream ? stream->fd : STDOUT_FILENO, 0, {0}, 0 };
    int n = format_core(&s, format, args);
    sink_flush(&s);
    return n;
}

int fprintf(FILE* stream, const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int result = vfprintf(stream, format, args);
    __builtin_va_end(args);
    return result;
}

int vprintf(const char* format, __builtin_va_list args) {
    return vfprintf(stdout, format, args);
}

int printf(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int result = vfprintf(stdout, format, args);
    __builtin_va_end(args);
    return result;
}

// Print string with newline
int puts(const char* str) {
    int len = strlen(str);
    write(STDOUT_FILENO, str, len);
    write(STDOUT_FILENO, "\n", 1);
    return len + 1;
}

// Print single character
int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

// Get single character (stub)
int getchar(void) {
    char c;
    while (1) {
        long result = read(STDIN_FILENO, &c, 1);
        if (result > 0) {
            return (unsigned char)c;
        }
        if (result < 0) {
            return -1;
        }
        yield();
    }
}

// ============================================================================
// FILE OPERATIONS
// ============================================================================

// Parse mode string for fopen
static int parse_mode(const char* mode, int* flags, int* append_mode) {
    if (!mode || !mode[0]) {
        return -1;
    }

    *append_mode = 0;

    switch (mode[0]) {
        case 'r':
            *flags = O_RDONLY;
            if (mode[1] == '+') {
                *flags = O_RDWR;
            }
            break;
        case 'w':
            *flags = O_WRONLY | O_CREAT | O_TRUNC;
            if (mode[1] == '+') {
                *flags = O_RDWR | O_CREAT | O_TRUNC;
            }
            break;
        case 'a':
            *flags = O_WRONLY | O_CREAT | O_APPEND;
            *append_mode = 1;
            if (mode[1] == '+') {
                *flags = O_RDWR | O_CREAT | O_APPEND;
            }
            break;
        default:
            return -1;
    }

    return 0;
}

// Open file
FILE* fopen(const char* filename, const char* mode) {
    if (!filename || !mode) {
        return NULL;
    }

    int flags;
    int append_mode;
    if (parse_mode(mode, &flags, &append_mode) < 0) {
        return NULL;
    }

    int fd = open(filename, flags, 0644);
    if (fd < 0) {
        return NULL;
    }

    FILE* stream = (FILE*)malloc(sizeof(FILE));
    if (!stream) {
        close(fd);
        return NULL;
    }

    stream->fd = fd;
    stream->flags = 0;
    stream->error = 0;
    stream->eof = 0;
    stream->buffer = NULL;
    stream->buf_size = 0;
    stream->buf_pos = 0;
    stream->buf_count = 0;
    stream->buf_mode = _IOFBF;  // Default to fully buffered

    if (flags & O_RDONLY || flags & O_RDWR) {
        stream->flags |= _FILE_READ;
    }
    if (flags & O_WRONLY || flags & O_RDWR) {
        stream->flags |= _FILE_WRITE;
    }

    // Allocate default 8KB buffer for efficiency
    stream->buffer = (char*)malloc(BUFSIZ);
    if (stream->buffer) {
        stream->buf_size = BUFSIZ;
    } else {
        // If malloc fails, fall back to unbuffered mode
        stream->buf_mode = _IONBF;
    }

    // If in append mode, seek to end
    if (append_mode) {
        lseek(fd, 0, SEEK_END);
    }

    return stream;
}

// Close file
int fclose(FILE* stream) {
    if (!stream) {
        return EOF;
    }

    // Don't close standard streams
    if (stream == stdin || stream == stdout || stream == stderr) {
        return 0;
    }

    fflush(stream);

    int result = close(stream->fd);

    if (stream->buffer) {
        free(stream->buffer);
    }

    free(stream);

    return (result < 0) ? EOF : 0;
}

// Read from file
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }

    if (!(stream->flags & _FILE_READ)) {
        stream->error = 1;
        return 0;
    }

    size_t total_bytes = size * nmemb;
    char* dest = (char*)ptr;

    // If unbuffered or no buffer, read directly
    if (stream->buf_mode == _IONBF || !stream->buffer) {
        size_t bytes_read = 0;
        while (bytes_read < total_bytes) {
            ssize_t result = read(stream->fd, dest + bytes_read, total_bytes - bytes_read);
            if (result < 0) {
                stream->error = 1;
                break;
            }
            if (result == 0) {
                stream->eof = 1;
                stream->flags |= _FILE_EOF;
                break;
            }
            bytes_read += result;
        }
        return bytes_read / size;
    }

    // Buffered read
    size_t bytes_read = 0;
    while (bytes_read < total_bytes) {
        // If buffer is empty, refill it
        if (stream->buf_pos >= stream->buf_count) {
            ssize_t result = read(stream->fd, stream->buffer, stream->buf_size);
            if (result < 0) {
                stream->error = 1;
                break;
            }
            if (result == 0) {
                stream->eof = 1;
                stream->flags |= _FILE_EOF;
                break;
            }
            stream->buf_count = (size_t)result;
            stream->buf_pos = 0;
        }

        // Copy from buffer to destination
        size_t bytes_available = stream->buf_count - stream->buf_pos;
        size_t bytes_remaining = total_bytes - bytes_read;
        size_t to_copy = (bytes_remaining < bytes_available) ? bytes_remaining : bytes_available;

        for (size_t i = 0; i < to_copy; i++) {
            dest[bytes_read++] = stream->buffer[stream->buf_pos++];
        }
    }

    return bytes_read / size;
}

// Write to file
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }

    if (!(stream->flags & _FILE_WRITE)) {
        stream->error = 1;
        return 0;
    }

    size_t total_bytes = size * nmemb;
    const char* src = (const char*)ptr;

    // If unbuffered or no buffer, write directly
    if (stream->buf_mode == _IONBF || !stream->buffer) {
        size_t bytes_written = 0;
        while (bytes_written < total_bytes) {
            ssize_t result = write(stream->fd, src + bytes_written, total_bytes - bytes_written);
            if (result < 0) {
                stream->error = 1;
                break;
            }
            bytes_written += result;
        }
        return bytes_written / size;
    }

    // Buffered write
    size_t bytes_written = 0;
    while (bytes_written < total_bytes) {
        size_t space_available = stream->buf_size - stream->buf_count;
        size_t bytes_remaining = total_bytes - bytes_written;
        size_t to_copy = (bytes_remaining < space_available) ? bytes_remaining : space_available;

        // Copy to buffer
        for (size_t i = 0; i < to_copy; i++) {
            stream->buffer[stream->buf_count++] = src[bytes_written++];
        }

        // Check if we need to flush
        int should_flush = 0;

        // Fully buffered: flush when buffer is full
        if (stream->buf_mode == _IOFBF && stream->buf_count >= stream->buf_size) {
            should_flush = 1;
        }

        // Line buffered: flush when we encounter a newline
        if (stream->buf_mode == _IOLBF) {
            for (size_t i = stream->buf_count - to_copy; i < stream->buf_count; i++) {
                if (stream->buffer[i] == '\n') {
                    should_flush = 1;
                    break;
                }
            }
        }

        if (should_flush) {
            if (fflush(stream) != 0) {
                break;
            }
        }
    }

    return bytes_written / size;
}

// Seek in file
int fseek(FILE* stream, long offset, int whence) {
    if (!stream) {
        return -1;
    }

    // Flush any pending writes
    if ((stream->flags & _FILE_WRITE) && stream->buf_count > 0) {
        if (fflush(stream) != 0) {
            return -1;
        }
    }

    // Invalidate read buffer
    if (stream->flags & _FILE_READ) {
        stream->buf_pos = 0;
        stream->buf_count = 0;
    }

    stream->eof = 0;
    stream->flags &= ~_FILE_EOF;

    off_t result = lseek(stream->fd, offset, whence);
    return (result < 0) ? -1 : 0;
}

// Get current file position
long ftell(FILE* stream) {
    if (!stream) {
        return -1L;
    }

    off_t pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos < 0) {
        return -1L;
    }

    // Adjust for buffered data
    if (stream->flags & _FILE_WRITE) {
        // Account for unflushed write buffer
        pos += stream->buf_count;
    } else if (stream->flags & _FILE_READ) {
        // Account for unconsumed read buffer
        pos -= (stream->buf_count - stream->buf_pos);
    }

    return (long)pos;
}

// Rewind to beginning of file
void rewind(FILE* stream) {
    if (stream) {
        fseek(stream, 0L, SEEK_SET);
        stream->error = 0;
        stream->eof = 0;
        stream->flags &= ~(_FILE_ERROR | _FILE_EOF);
    }
}

// Get file position
int fgetpos(FILE* stream, fpos_t* pos) {
    if (!stream || !pos) {
        return -1;
    }

    long p = ftell(stream);
    if (p < 0) {
        return -1;
    }

    *pos = p;
    return 0;
}

// Set file position
int fsetpos(FILE* stream, const fpos_t* pos) {
    if (!stream || !pos) {
        return -1;
    }

    return fseek(stream, *pos, SEEK_SET);
}

// ============================================================================
// CHARACTER I/O
// ============================================================================

// Read character from file
int fgetc(FILE* stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1) {
        return EOF;
    }
    return c;
}

// Write character to file
int fputc(int c, FILE* stream) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, stream) != 1) {
        return EOF;
    }
    return ch;
}

// Read line from file
char* fgets(char* s, int size, FILE* stream) {
    if (!s || size <= 0 || !stream) {
        return NULL;
    }

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) {
                return NULL;
            }
            break;
        }

        s[i++] = (char)c;

        if (c == '\n') {
            break;
        }
    }

    s[i] = '\0';
    return s;
}

// Write string to file
int fputs(const char* s, FILE* stream) {
    if (!s || !stream) {
        return EOF;
    }

    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) {
        return EOF;
    }

    return 0;
}

// Unget character (simplified - no buffer)
int ungetc(int c, FILE* stream) {
    if (!stream || c == EOF) {
        return EOF;
    }

    // Simplified: just seek back one byte
    if (lseek(stream->fd, -1, SEEK_CUR) < 0) {
        return EOF;
    }

    return c;
}

// ============================================================================
// FORMATTED INPUT
// ============================================================================

// Simple scanf implementation (basic support)
int fscanf(FILE* stream, const char* format, ...) {
    if (!stream || !format) {
        return EOF;
    }

    // For now, just a stub that returns EOF
    // Full implementation would require parsing the format string
    // and reading from the stream accordingly
    (void)format;
    return EOF;
}

int scanf(const char* format, ...) {
    return fscanf(stdin, format);
}

int sscanf(const char* str, const char* format, ...) {
    // Stub implementation
    (void)str;
    (void)format;
    return EOF;
}

// ============================================================================
// ERROR HANDLING
// ============================================================================

// Clear error indicators
void clearerr(FILE* stream) {
    if (stream) {
        stream->error = 0;
        stream->eof = 0;
        stream->flags &= ~(_FILE_ERROR | _FILE_EOF);
    }
}

// Test end-of-file indicator
int feof(FILE* stream) {
    return stream ? stream->eof : 0;
}

// Test error indicator
int ferror(FILE* stream) {
    return stream ? stream->error : 0;
}

// Print error message: "<s>: <strerror(errno)>\n"
void perror(const char* s) {
    if (s && *s) {
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    } else {
        fprintf(stderr, "%s\n", strerror(errno));
    }
}

// Return the underlying file descriptor of a stream.
int fileno(FILE* stream) {
    return stream ? stream->fd : -1;
}

// ============================================================================
// BUFFERING
// ============================================================================

// Set buffer
int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    if (!stream) {
        return -1;
    }

    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        return -1;
    }

    // Free existing buffer if we allocated it
    if (stream->buffer && stream->buf_size > 0) {
        free(stream->buffer);
        stream->buffer = NULL;
        stream->buf_size = 0;
    }

    stream->buf_mode = mode;

    if (mode == _IONBF) {
        return 0;
    }

    if (buf) {
        stream->buffer = buf;
        stream->buf_size = size;
    } else if (size > 0) {
        stream->buffer = (char*)malloc(size);
        if (!stream->buffer) {
            return -1;
        }
        stream->buf_size = size;
    }

    stream->buf_pos = 0;
    stream->buf_count = 0;

    return 0;
}

// Set buffer (simple version)
void setbuf(FILE* stream, char* buf) {
    if (buf) {
        setvbuf(stream, buf, _IOFBF, BUFSIZ);
    } else {
        setvbuf(stream, NULL, _IONBF, 0);
    }
}

// Flush buffer
int fflush(FILE* stream) {
    if (!stream) {
        // Flush all streams
        fflush(stdout);
        fflush(stderr);
        return 0;
    }

    if (stream->buffer && stream->buf_count > 0 && (stream->flags & _FILE_WRITE)) {
        ssize_t written = write(stream->fd, stream->buffer, stream->buf_count);
        if (written < 0) {
            stream->error = 1;
            return EOF;
        }
        stream->buf_count = 0;
        stream->buf_pos = 0;
    }

    return 0;
}
