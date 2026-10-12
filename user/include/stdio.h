#ifndef _STDIO_H
#define _STDIO_H

#include <fmt.h>        /* kvcbprintf/ksnprintf, shared with the kernel */
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 256
#define FOPEN_MAX 64
#define L_tmpnam 32
#define TMP_MAX 10000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct FILE FILE;
typedef long fpos_t;

extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int fclose(FILE *f);
int fflush(FILE *f);                    /* NULL: every open stream */
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
int fileno(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
ssize_t getline(char **line, size_t *cap, FILE *f);
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
FILE *tmpfile(void);
char *tmpnam(char *s);
void perror(const char *s);

int printf(const char *fmt, ...) __printf_like(1, 2);
int fprintf(FILE *f, const char *fmt, ...) __printf_like(2, 3);
int dprintf(int fd, const char *fmt, ...) __printf_like(2, 3);
int sprintf(char *buf, const char *fmt, ...) __printf_like(2, 3);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int asprintf(char **out, const char *fmt, ...) __printf_like(2, 3);
int vasprintf(char **out, const char *fmt, va_list ap);
#define snprintf ksnprintf
#define vsnprintf kvsnprintf

int scanf(const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vscanf(const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);

/* Eunuchs extras, used by the system's own programs */
void fflush_stdout(void);
int fprintf_fd(int fd, const char *fmt, ...) __printf_like(2, 3);
#define eprintf(...) fprintf_fd(2, __VA_ARGS__)

#endif
