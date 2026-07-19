/* libc_shim.h -- bionic-compatible libc wrappers for libsotn.so.
 * MIT license; see LICENSE. */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

// fortify
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen);

// misc bionic
long syscall_fake(long number, ...);
void sincos_fake(double x, double *s, double *c);
int clock_gettime_fake(int clk, void *ts);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
long sysconf_fake(int name);

// signals (stubbed -- the engine installs handlers we never raise)
int sigaction_fake(int sig, const void *act, void *oldact);
void *signal_fake(int sig, void *handler);
int sigaddset_fake(void *set, int sig);
int sigemptyset_fake(void *set);
int pthread_sigmask_fake(int how, const void *set, void *oldset);

int stat_fake(const char *path, void *st);

// locale (ignore the locale arg, use the C versions)
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);

int posix_memalign_fake(void **out, size_t align, size_t size);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int fseek_fake(FILE *f, long off, int whence);
int putchar_fake(int c);
int puts_fake(const char *s);
FILE *fopen_fake(const char *path, const char *mode);
int open_fake(const char *path, int flags, ...);
int access_fake(const char *path, int mode);
ssize_t readlink_fake(const char *path, char *buf, size_t bufsz);

// android-style sandbox: rebase stray absolute paths into save_root
const char *sandbox_path(const char *path, char *buf, size_t sz);

// extra stdio wrappers guarding the fake std streams (Godot/libc++ touch more
// of the FILE surface than NBA Jam did)
int is_fake_std_file(const void *f);
char *fgets_fake(char *s, int n, FILE *f);
int getc_fake(FILE *f);
int ungetc_fake(int c, FILE *f);
void setbuf_fake(FILE *f, char *buf);
void rewind_fake(FILE *f);
int fgetpos_fake(FILE *f, void *pos);
int fsetpos_fake(FILE *f, const void *pos);
int fseeko_fake(FILE *f, int64_t off, int whence);
int64_t ftello_fake(FILE *f);
int fileno_fake(FILE *f);
int fputs_fake(const char *s, FILE *f);
int vprintf_fake(const char *fmt, va_list va);
long ftell_fake(FILE *f);
int feof_fake(FILE *f);

// OBB read-ahead wrappers (transparent for non-OBB descriptors)
ssize_t read_fake(int fd, void *dst, size_t count);
off_t lseek_fake(int fd, off_t off, int whence);
int close_fake(int fd);

// running total of bytes served from the OBB (for load detection)
uint64_t obb_bytes_total(void);
int obb_fallback_path(const char *path, char *out, size_t outsz);
const char *obb_resolve(const char *path, char *buf, size_t bufsz);

// ANativeWindow -> NWindow
void *ANativeWindow_fromSurface_fake(void *env, void *surface);
void ANativeWindow_release_fake(void *win);
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format);

// POSIX semaphores (pointer-indirected)
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_trywait_fake(void **s);
int sem_getvalue_fake(void **s, int *val);

#endif
