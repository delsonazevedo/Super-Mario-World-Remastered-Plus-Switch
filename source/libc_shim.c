/* libc_shim.c -- bionic<->newlib libc wrappers for libsotn.so. Converting
 * wrappers where the ABIs differ; matches are forwarded from imports.c.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"
#include "util.h"

// fortify (_chk): ignore the object-size argument
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy(dst, src, n);
}
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memmove(dst, src, n);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen; return read(fd, buf, count);
}

static int gettid_fake(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID) return gettid_fake();
  errno = ENOSYS;
  return -1;
}

void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// bionic clockids (REALTIME=0, MONOTONIC=1, ...) differ from newlib's, so the
// engine's clock_gettime(0) was rejected as EINVAL -> uncaught std::system_error.
// Translate the id; fall back to the libnx tick so it never fails. bionic and
// newlib timespec match on arm64 LP64.
int clock_gettime_fake(int clk, void *ts_) {
  struct timespec *ts = ts_;
  if (!ts) { errno = EFAULT; return -1; }
  clockid_t real = (clk == 0 || clk == 5) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  if (clock_gettime(real, ts) == 0)
    return 0;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  ts->tv_sec = (int64_t)(ns / 1000000000ull);
  ts->tv_nsec = (int64_t)(ns % 1000000000ull);
  return 0;
}

void android_set_abort_message_fake(const char *msg) { (void)msg; }

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    case BIONIC_SC_PHYS_PAGES: return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}

// signals: the engine installs a crash handler we never trigger; stub them
int sigaction_fake(int sig, const void *act, void *oldact) {
  (void)sig; (void)act; (void)oldact; return 0;
}
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int sigaddset_fake(void *set, int sig) { (void)set; (void)sig; return 0; }
int sigemptyset_fake(void *set) { (void)set; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *oldset) {
  (void)how; (void)set; (void)oldset; return 0;
}

// struct stat conversion (bionic aarch64 layout)
struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

int stat_fake(const char *path, void *st) {
  struct stat in;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = obb_resolve(path, buf, sizeof(buf));
  int rc = stat(p, &in);
#if VERBOSE_IO
  debugPrintf("stat(\"%s\"%s) -> %d\n", path, p != path ? " [obb->main.obb]" : "", rc);
#endif
  if (rc != 0) return -1;
  struct bionic_stat *out = st;
  memset(out, 0, sizeof(*out));
  out->st_dev = in.st_dev; out->st_ino = in.st_ino;
  out->st_mode = in.st_mode; out->st_nlink = in.st_nlink;
  out->st_uid = in.st_uid; out->st_gid = in.st_gid;
  out->st_rdev = in.st_rdev; out->st_size = in.st_size;
  out->st_blksize = in.st_blksize; out->st_blocks = in.st_blocks;
  out->st_atim.tv_sec = in.st_atime;
  out->st_mtim.tv_sec = in.st_mtime;
  out->st_ctim.tv_sec = in.st_ctime;
  return 0;
}

// locale: ignore the locale argument, use the C versions
void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base; return (void *)1;
}
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc; return strtold(s, end);
}
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoll(s, end, base);
}
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoull(s, end, base);
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// stdio over the fake bionic __sF: libc++/SDL bind std streams to &__sF[N];
// these absorb accesses to those fake FILEs and forward everything else.
uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

// Android-style filesystem sandbox: the app cannot touch "/" on a real
// device, and with the faked cwd of "/" godot builds cwd-derived write paths
// like "/saves/...". Rebase absolute paths that are outside the app's own
// directories into save_root; app paths (/switch/...) and device-prefixed
// paths pass through untouched.
const char *sandbox_path(const char *path, char *buf, size_t sz) {
  if (!path || path[0] != '/') return path;
  if (strncmp(path, "/switch", 7) == 0) return path;
  if (strncmp(path, "/dev", 4) == 0 || strncmp(path, "/proc", 5) == 0) return path;
  snprintf(buf, sz, "%s%s", config.save_root, path);
  return buf;
}

// /dev/urandom | /dev/random: mbedtls seeds its CTR_DRBG from it (fopen +
// fread and/or open + read). Serve both through the system csrng.
static uint8_t urandom_marker; // its address doubles as the fake FILE *
#define URANDOM_FD 0x7F0FA7E

static int is_urandom_path(const char *p) {
  return p && strncmp(p, "/dev/", 5) == 0 && strstr(p + 5, "random") != NULL;
}
static int is_urandom_file(const void *f) { return f == (const void *)&urandom_marker; }

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}

// NULL-safe: the engine fread()s into a malloc'd buffer without checking the
// FILE* or the buffer, so guard against a write-to-0x0 Data Abort.
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_urandom_file(f)) {
    if (size && n) randomGet(ptr, size * n);
    return n;
  }
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (!f) return -1;
  if (is_fake_file(f)) return c;
  return fputc(c, f);
}
int fflush_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return fclose(f);
}
int ferror_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return ferror(f);
}
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) {
  if (!f || is_fake_file(f)) return -1;
  if (is_urandom_file(f)) return 0;
  return fseek(f, off, whence);
}
int putchar_fake(int c) { return c; }
int puts_fake(const char *s) { (void)s; return 0; }

// remaining FILE surface Godot/libc++ touch, with fake-std-stream guards
int is_fake_std_file(const void *f) { return is_fake_file(f); }

char *fgets_fake(char *s, int n, FILE *f) {
  if (!f || !s || is_fake_file(f) || is_urandom_file(f)) return NULL;
  return fgets(s, n, f);
}
int getc_fake(FILE *f) {
  if (is_urandom_file(f)) {
    uint8_t b;
    randomGet(&b, 1);
    return b;
  }
  if (!f || is_fake_file(f)) return -1; // EOF
  return getc(f);
}
int ungetc_fake(int c, FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return -1;
  return ungetc(c, f);
}
void setbuf_fake(FILE *f, char *buf) {
  if (f && !is_fake_file(f) && !is_urandom_file(f)) setbuf(f, buf);
}
void rewind_fake(FILE *f) {
  if (f && !is_fake_file(f) && !is_urandom_file(f)) rewind(f);
}
// bionic fpos_t is a 64-bit offset; carry it as one
int fgetpos_fake(FILE *f, void *pos) {
  if (is_urandom_file(f)) { if (pos) *(int64_t *)pos = 0; return 0; }
  if (!f || !pos || is_fake_file(f)) return -1;
  long p = ftell(f);
  if (p < 0) return -1;
  *(int64_t *)pos = p;
  return 0;
}
int fsetpos_fake(FILE *f, const void *pos) {
  if (is_urandom_file(f)) return 0;
  if (!f || !pos || is_fake_file(f)) return -1;
  return fseek(f, (long)*(const int64_t *)pos, SEEK_SET);
}
int fseeko_fake(FILE *f, int64_t off, int whence) {
  if (is_urandom_file(f)) return 0;
  if (!f || is_fake_file(f)) return -1;
  return fseek(f, (long)off, whence);
}
int64_t ftello_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return is_urandom_file(f) ? 0 : -1;
  return ftell(f);
}
long ftell_fake(FILE *f) {
  if (is_urandom_file(f)) return 0;
  if (!f || is_fake_file(f)) return -1;
  return ftell(f);
}
int feof_fake(FILE *f) {
  if (is_urandom_file(f)) return 0; // never runs dry
  if (!f || is_fake_file(f)) return 1;
  return feof(f);
}
int fileno_fake(FILE *f) {
  if (is_urandom_file(f)) return URANDOM_FD;
  if (!f || is_fake_file(f)) return -1;
  return fileno(f);
}
int fputs_fake(const char *s, FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return fputs(s ? s : "", f);
}
int vprintf_fake(const char *fmt, va_list va) {
#if DEBUG_LOG
  char buf[0x800];
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  debugPrintf("%s", buf);
  return n;
#else
  (void)fmt; (void)va;
  return 0;
#endif
}

// The game builds the standard Android OBB name (main.<versionCode>.<pkg>.obb)
// and opens it under getObbPath(). Our release ships it as plain "main.obb", so
// when a "main.*.obb" open fails, retry with "main.obb" in the same directory.
// Returns 1 and fills `out` when a rewrite applies.
int obb_fallback_path(const char *path, char *out, size_t outsz) {
  if (!path) return 0;
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  size_t blen = strlen(base);
  if (strncmp(base, "main.", 5) != 0 || blen < 5 || strcmp(base + blen - 4, ".obb") != 0)
    return 0;
  if (blen == 8) return 0; // already exactly "main.obb"
  if (slash)
    snprintf(out, outsz, "%.*s/main.obb", (int)(slash - path), path);
  else
    snprintf(out, outsz, "main.obb");
  return 1;
}

// If `path` is a versioned "main.*.obb", resolve it to the shipped "main.obb"
// in the same directory (our release ships it un-versioned). No stat/access
// probe here: it's on the hot asset-load path, so keep it syscall-free.
const char *obb_resolve(const char *path, char *buf, size_t bufsz) {
  if (path && obb_fallback_path(path, buf, bufsz)) return buf;
  return path;
}

// OBB read-ahead helpers (defined below)
static int path_is_obb(const char *p);
static void obb_track(int fd);

// open() with the OBB-name fallback (PhysFS on Android opens archives via the
// POSIX open path, not fopen).
int open_fake(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  if (is_urandom_path(path))
    return URANDOM_FD;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = ((flags & O_ACCMODE) == O_RDONLY) ? obb_resolve(path, buf, sizeof(buf)) : path;
  int fd = open(p, flags, mode);
  if (fd >= 0 && (flags & O_ACCMODE) == O_RDONLY && path_is_obb(p))
    obb_track(fd); // enable read-ahead for this OBB descriptor
#if VERBOSE_IO
  debugPrintf("open(\"%s\"%s) flags=%x -> %d\n", path, p != path ? " [obb->main.obb]" : "", flags, fd);
#endif
  return fd;
}

int access_fake(const char *path, int mode) {
  // The game's bundled PhysFS probes access("/proc") to decide whether it can
  // resolve its own executable path via readlink("/proc/self/exe"). Switch has
  // no /proc, so claim it exists and satisfy the readlink below; otherwise
  // PHYSFS_init's calcBaseDir returns NULL and PHYSFS_init fails, leaving its
  // stateLock NULL -> PHYSFS_mount then crashes locking a NULL mutex.
  if (path && strcmp(path, "/proc") == 0)
    return 0;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = obb_resolve(path, buf, sizeof(buf));
  return access(p, mode);
}

// ---------------------------------------------------------------------------
// OBB read-ahead: the game reads its (stored, uncompressed) OBB through PhysFS
// with many small read()/lseek() calls. Each is a round trip to the SD card.
// We wrap read()/lseek()/close() for OBB file descriptors with a large
// per-fd read-ahead buffer, collapsing the small reads into few big ones.
// ---------------------------------------------------------------------------

#define OBB_NFD 8
#define OBB_RA_MIN (32 * 1024)
#define OBB_RA_MAX (256 * 1024)
#define OBB_BUF_SZ (OBB_RA_MAX + 0x2000) // + alignment slack

typedef struct {
  int fd;            // -1 = free slot
  off_t pos;         // logical file position
  off_t size;        // file size
  off_t buf_start;   // file offset of the buffered region
  size_t buf_len;    // valid bytes in buf
  uint8_t *buf;      // OBB_BUF_SZ, 0x1000-aligned (allocated once per slot)
  Mutex lock;
  size_t ra;         // adaptive read-ahead window (grows on sequential access)
  off_t seq_next;    // file offset expected for the next sequential read
} ObbFd;

static ObbFd s_obb[OBB_NFD];
static Mutex s_obb_table_lock;
static int s_obb_inited;

// global I/O instrumentation (per-fd is too short-lived to be meaningful)
static uint64_t g_obb_opens;    // times the OBB was opened
static uint64_t g_obb_reads;    // read() calls the game made on the OBB
static uint64_t g_obb_refills;  // actual SD reads we issued (read-ahead misses)
static uint64_t g_obb_bytes;    // bytes served to the game from the OBB
static uint64_t g_obb_sd_bytes; // bytes actually read from the SD (over-read = this - served)
static uint64_t g_fopen_calls;  // total fopen() attempts
static uint64_t g_fopen_fail;   // fopen() attempts that missed (loose-file probes)
#if DEBUG_LOG
static uint64_t g_next_bytes;   // next g_obb_bytes milestone to log at
static uint64_t g_next_fopen;   // next g_fopen_calls milestone to log at
#endif

// total bytes served from the OBB so far; the main loop samples this to detect
// "loading" (heavy OBB reads) and boost the CPU during those windows.
uint64_t obb_bytes_total(void) { return g_obb_bytes; }

static void io_maybe_log(void) {
#if DEBUG_LOG
  if (g_obb_bytes >= g_next_bytes || g_fopen_calls >= g_next_fopen) {
    g_next_bytes = g_obb_bytes + 16 * 1024 * 1024;
    g_next_fopen = g_fopen_calls + 4000;
    debugPrintf("[io] obb: opens=%llu reads=%llu refills=%llu served=%lluMB sd=%lluMB | fopen: %llu (%llu miss)\n",
                (unsigned long long)g_obb_opens, (unsigned long long)g_obb_reads,
                (unsigned long long)g_obb_refills, (unsigned long long)(g_obb_bytes >> 20),
                (unsigned long long)(g_obb_sd_bytes >> 20),
                (unsigned long long)g_fopen_calls, (unsigned long long)g_fopen_fail);
  }
#endif
}

static void obb_io_init(void) {
  if (!s_obb_inited) {
    mutexInit(&s_obb_table_lock);
    for (int i = 0; i < OBB_NFD; i++) s_obb[i].fd = -1;
    s_obb_inited = 1;
  }
}

static ObbFd *obb_find(int fd) {
  if (!s_obb_inited) return NULL;
  for (int i = 0; i < OBB_NFD; i++) if (s_obb[i].fd == fd) return &s_obb[i];
  return NULL;
}

static int path_is_obb(const char *p) {
  const char *s = strrchr(p, '/');
  s = s ? s + 1 : p;
  return strcmp(s, "main.obb") == 0;
}

static void obb_track(int fd) {
  obb_io_init();
  mutexLock(&s_obb_table_lock);
  for (int i = 0; i < OBB_NFD; i++) {
    if (s_obb[i].fd < 0) {
      if (!s_obb[i].buf) { s_obb[i].buf = memalign(0x1000, OBB_BUF_SZ); mutexInit(&s_obb[i].lock); }
      s_obb[i].pos = 0; s_obb[i].buf_start = 0; s_obb[i].buf_len = 0;
      s_obb[i].ra = OBB_RA_MIN; s_obb[i].seq_next = -1;
      struct stat st;
      s_obb[i].size = (fstat(fd, &st) == 0) ? st.st_size : 0;
      if (s_obb[i].buf) { s_obb[i].fd = fd; g_obb_opens++; } // publish last
      break;
    }
  }
  mutexUnlock(&s_obb_table_lock);
}

ssize_t read_fake(int fd, void *dst, size_t count) {
  if (fd == URANDOM_FD) {
    if (dst && count) randomGet(dst, count);
    return (ssize_t)count;
  }
  ObbFd *o = obb_find(fd);
  if (!o) return read(fd, dst, count);
  mutexLock(&o->lock);
  uint8_t *out = dst;
  size_t done = 0;
  while (done < count && o->pos < o->size) {
    if (o->pos < o->buf_start || o->pos >= o->buf_start + (off_t)o->buf_len) {
      // adaptive window: grow while the game reads sequentially, shrink on a
      // seek, so scattered tiny reads don't over-read hundreds of MB.
      if (o->pos == o->seq_next) { o->ra <<= 1; if (o->ra > OBB_RA_MAX) o->ra = OBB_RA_MAX; }
      else                        { o->ra = OBB_RA_MIN; }
      off_t start = o->pos & ~(off_t)0xFFF;         // page-align the backing read
      size_t rsize = o->ra + (size_t)(o->pos - start); // cover the alignment slack
      if (rsize > OBB_BUF_SZ) rsize = OBB_BUF_SZ;
      if (lseek(fd, start, SEEK_SET) != start) break;
      ssize_t got = read(fd, o->buf, rsize);
      if (got <= 0) break;
      o->buf_start = start; o->buf_len = (size_t)got;
      o->seq_next = start + (off_t)got;             // next sequential read lands here
      g_obb_refills++; g_obb_sd_bytes += (uint64_t)got;
    }
    size_t off = (size_t)(o->pos - o->buf_start);
    size_t avail = o->buf_len - off;
    size_t n = count - done;
    if (n > avail) n = avail;
    memcpy(out + done, o->buf + off, n);
    done += n; o->pos += n;
  }
  g_obb_reads++; g_obb_bytes += done;
  io_maybe_log();
  mutexUnlock(&o->lock);
  return (ssize_t)done;
}

off_t lseek_fake(int fd, off_t off, int whence) {
  ObbFd *o = obb_find(fd);
  if (!o) return lseek(fd, off, whence);
  mutexLock(&o->lock);
  off_t np = (whence == SEEK_SET) ? off
           : (whence == SEEK_CUR) ? o->pos + off
                                  : o->size + off; // SEEK_END
  if (np < 0) np = 0;
  o->pos = np;
  mutexUnlock(&o->lock);
  return np;
}

int close_fake(int fd) {
  if (fd == URANDOM_FD) return 0;
  ObbFd *o = obb_find(fd);
  if (o) { mutexLock(&s_obb_table_lock); o->fd = -1; mutexUnlock(&s_obb_table_lock); }
  return close(fd);
}

// readlink("/proc/self/exe" and friends): hand PhysFS a plausible absolute
// executable path so calcBaseDir yields "<data_root>/" as the base dir.
ssize_t readlink_fake(const char *path, char *buf, size_t bufsz) {
  if (path && strncmp(path, "/proc", 5) == 0) {
    int n = snprintf(buf, bufsz, "%s/smwr.nro", config.data_root);
    if (n < 0) return -1;
    if ((size_t)n > bufsz) n = (int)bufsz; // readlink returns the truncated count
    return n;
  }
  return readlink(path, buf, bufsz);
}

// Some data lives in a region/language subfolder (pspbin/eu/, pack/jp/,
// sound/xa/en/) the engine sometimes omits; retry with each inserted.
static FILE *fopen_region_fallback(const char *path, const char *mode) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return NULL;
  static const char *regions[] = { "eu", "us", "jp", "en" };
  char cand[640];
  const int dirlen = (int)(slash - path);
  for (unsigned i = 0; i < sizeof(regions) / sizeof(*regions); i++) {
    snprintf(cand, sizeof(cand), "%.*s/%s/%s", dirlen, path, regions[i], slash + 1);
    FILE *f = fopen(cand, mode);
    if (f) return f;
  }
  return NULL;
}

// large stream buffer: the engine issues many small reads/seeks against the
// game archives and fsdev round trips dominate otherwise.
// Negative directory cache: the game probes each asset in ~6 override
// directories, 5 of which don't exist on the SD (their content lives in the
// OBB). A failed fopen into a missing directory still costs an SD directory
// lookup; remember which directories exist and short-circuit the rest.
#define DIRCACHE_N 64
static struct { char dir[256]; int exists; } s_dircache[DIRCACHE_N];
static int s_dircache_n;
static Mutex s_dircache_lock; // zero-initialized == unlocked (libnx)

static int dir_exists_cached(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return 1; // no dir / root -> treat as existing
  size_t len = (size_t)(slash - path);
  if (len >= 256) return 1;
  char dir[256];
  memcpy(dir, path, len);
  dir[len] = 0;

  mutexLock(&s_dircache_lock);
  for (int i = 0; i < s_dircache_n; i++)
    if (!strcmp(s_dircache[i].dir, dir)) { int e = s_dircache[i].exists; mutexUnlock(&s_dircache_lock); return e; }
  struct stat st;
  int e = (stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
  if (s_dircache_n < DIRCACHE_N) {
    strcpy(s_dircache[s_dircache_n].dir, dir);
    s_dircache[s_dircache_n].exists = e;
    s_dircache_n++;
  }
  mutexUnlock(&s_dircache_lock);
  return e;
}

FILE *fopen_fake(const char *path, const char *mode) {
  if (!path) return NULL;
  if (is_urandom_path(path))
    return (FILE *)&urandom_marker;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  // read probes into a non-existent directory can't succeed -> skip the SD hit
  if (strchr(mode, 'r') && !dir_exists_cached(path)) {
    g_fopen_calls++; g_fopen_fail++; io_maybe_log();
    return NULL;
  }
  FILE *f = fopen(path, mode);
  if (!f && strchr(mode, 'r')) {
    f = fopen_region_fallback(path, mode);
    if (!f) {
      char cand[640];
      if (obb_fallback_path(path, cand, sizeof(cand)))
        f = fopen(cand, mode);
    }
  }
  if (f && strchr(mode, 'r'))
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
  g_fopen_calls++;
  if (!f) g_fopen_fail++;
  io_maybe_log();
#if VERBOSE_IO
  debugPrintf("fopen(\"%s\", \"%s\") -> %p\n", path, mode, (void *)f);
#endif
  return f;
}

// ANativeWindow -> NWindow: hand SDL the real Switch window so mesa's
// eglCreateWindowSurface lands on it.
void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  return win;
}
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  if (w > 0 && h > 0) nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// POSIX semaphores via pointer indirection (bionic sem_t is 16 bytes on LP64,
// so a heap FakeSem* fits in the caller's storage)
typedef struct { Semaphore sem; } FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  if (!fs) return -1;
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) {
  if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_wait_fake(void **s) {
  if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0;
  errno = EAGAIN;
  return -1;
}
int sem_getvalue_fake(void **s, int *val) {
  *val = (s && *s) ? (int)((FakeSem *)*s)->sem.count : 0;
  return 0;
}
