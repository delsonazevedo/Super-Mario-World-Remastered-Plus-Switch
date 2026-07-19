/* imports.c -- resolves the dynamic imports of libgodot_android.so (Godot
 * 4.6.dev4, arm64-v8a) and libc++_shared.so against newlib, host zlib, mesa
 * GLES3/EGL, and our shims. The import list is built from the actual UND
 * symbols of both libraries (reference/all_needs_wrapper.txt); C++ ABI symbols
 * (_Z.. and __cxa..) resolve module-to-module from libc++_shared's exports.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <fenv.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <zlib.h>
#include <switch.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "godot_shim.h"
#include "egl_shim.h"
#include "jni_fake.h"

// real libc/gcc symbols whose addresses we forward verbatim
extern int   __cxa_atexit(void (*)(void *), void *, void *);
extern void  __stack_chk_fail(void);

// ---------------------------------------------------------------------------
// bionic logging
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("[%s] %s\n", tag ? tag : "", string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// errno
// ---------------------------------------------------------------------------

static int *__errno_fake(void) { return &errno; }

// ---------------------------------------------------------------------------
// pthread: bionic's opaque types are zero-inited inline, so lazily back them
// with heap newlib objects stashed in the caller's first pointer slot.
// ---------------------------------------------------------------------------

#define BIONIC_RECURSIVE_MARK 0x8000
#define BIONIC_ERRCHECK_MARK  0x4000

static int to_bionic_errno(int e) {
  switch (e) {
    case 116: return 110; // ETIMEDOUT
    case 45:  return 35;  // EDEADLK
    default:  return e;
  }
}

// Bionic's pthread_mutex_t (40 B) and pthread_cond_t (48 B) are int32-array
// structs with 4-byte alignment. We stash the pointer to our real newlib
// backing object at the FIRST 8-ALIGNED offset inside that storage: exclusive
// loads (the CAS below) require natural alignment, and the struct is large
// enough that the aligned slot always fits. The static-initializer type mark
// (bionic stores it in the first int32) is read separately.
static inline uint64_t *lazy_slot(void *storage) {
  return (uint64_t *)(((uintptr_t)storage + 7) & ~(uintptr_t)7);
}

static pthread_mutex_t *make_mutex(int recursive) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return NULL;
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return NULL; }
  return m;
}

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = make_mutex(mutexattr && *mutexattr == 1);
  if (!m) return -1;
  __atomic_store_n(lazy_slot(uid), (uint64_t)(uintptr_t)m, __ATOMIC_RELEASE);
  return 0;
}

// Lazy backing-object creation must be race-free: two threads touching the
// same statically-initialized bionic mutex/cond concurrently would otherwise
// each allocate an object, and the loser locks/waits on a different object
// than everyone else (lost wakeups -> boot deadlocks in WorkerThreadPool).
static pthread_mutex_t *ensure_mutex(pthread_mutex_t **uid) {
  uint64_t *slot = lazy_slot(uid);
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (cur > 0x10000) return (pthread_mutex_t *)(uintptr_t)cur;
  const uint32_t mark = *(volatile uint32_t *)uid; // bionic type mark, 4-aligned
  pthread_mutex_t *m = make_mutex(mark == BIONIC_RECURSIVE_MARK || mark == BIONIC_ERRCHECK_MARK);
  if (!m) return NULL;
  if (__atomic_compare_exchange_n(slot, &cur, (uint64_t)(uintptr_t)m, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return m;
  pthread_mutex_destroy(m); // another thread won the race; use its object
  free(m);
  return (cur > 0x10000) ? (pthread_mutex_t *)(uintptr_t)cur : NULL;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (!uid) return 0;
  uint64_t *slot = lazy_slot(uid);
  uint64_t v = __atomic_exchange_n(slot, 0, __ATOMIC_ACQ_REL);
  if (v > 0x10000) {
    pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)v);
    free((void *)(uintptr_t)v);
  }
  return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? pthread_mutex_lock(m) : -1;
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? pthread_mutex_trylock(m) : -1;
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? pthread_mutex_unlock(m) : -1;
}

// race-free lazy condvar backing, same aligned-slot scheme as ensure_mutex
static pthread_cond_t *ensure_cond(pthread_cond_t **cnd) {
  uint64_t *slot = lazy_slot(cnd);
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (cur > 0x10000) return (pthread_cond_t *)(uintptr_t)cur;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return NULL;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return NULL; }
  if (__atomic_compare_exchange_n(slot, &cur, (uint64_t)(uintptr_t)c, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return c;
  pthread_cond_destroy(c); // another thread won the race
  free(c);
  return (cur > 0x10000) ? (pthread_cond_t *)(uintptr_t)cur : NULL;
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  return ensure_cond(cnd) ? 0 : -1;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  pthread_cond_t *c = ensure_cond(cnd);
  return c ? pthread_cond_broadcast(c) : -1;
}
static int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  pthread_cond_t *c = ensure_cond(cnd);
  return c ? pthread_cond_signal(c) : -1;
}
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (!cnd) return 0;
  uint64_t v = __atomic_exchange_n(lazy_slot(cnd), 0, __ATOMIC_ACQ_REL);
  if (v > 0x10000) {
    pthread_cond_destroy((pthread_cond_t *)(uintptr_t)v);
    free((void *)(uintptr_t)v);
  }
  return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  pthread_cond_t *c = ensure_cond(cnd);
  pthread_mutex_t *m = ensure_mutex(mtx);
  if (!c || !m) return -1;
  return pthread_cond_wait(c, m);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  pthread_cond_t *c = ensure_cond(cnd);
  pthread_mutex_t *m = ensure_mutex(mtx);
  if (!c || !m) return -1;
  return to_bionic_errno(pthread_cond_timedwait(c, m, t));
}

// waiters must block until the winner FINISHES the init routine (the old
// version let them race past while init was still running)
static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  int prev = __sync_val_compare_and_swap(once_control, 0, 1);
  if (prev == 0) {
    (*init_routine)();
    __sync_synchronize();
    *once_control = 2;
    return 0;
  }
  while (*once_control != 2)
    svcSleepThread(100 * 1000); // 0.1 ms
  return 0;
}

static int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
static int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// new engine threads need tpidr_el0 pointing at a stack-guard block first.
// each engine thread is also registered so the hang watchdog can dump it.
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

#define MAX_GAME_THREADS 64
static struct { Thread *thr; void *entry; int alive; } s_game_threads[MAX_GAME_THREADS];
static Mutex s_game_threads_lock;

int smwr_engine_threads(Thread **out_thr, void **out_entry, int max) {
  int n = 0;
  mutexLock(&s_game_threads_lock);
  for (int i = 0; i < MAX_GAME_THREADS && n < max; i++) {
    if (s_game_threads[i].alive && s_game_threads[i].thr) {
      out_thr[n] = s_game_threads[i].thr;
      out_entry[n] = s_game_threads[i].entry;
      n++;
    }
  }
  mutexUnlock(&s_game_threads_lock);
  return n;
}

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();

  int slot = -1;
  mutexLock(&s_game_threads_lock);
  for (int i = 0; i < MAX_GAME_THREADS; i++) {
    if (!s_game_threads[i].alive) {
      s_game_threads[i].thr = threadGetSelf();
      s_game_threads[i].entry = (void *)ts.entry;
      s_game_threads[i].alive = 1;
      slot = i;
      break;
    }
  }
  mutexUnlock(&s_game_threads_lock);

  void *ret = ts.entry(ts.arg);

  if (slot >= 0) {
    mutexLock(&s_game_threads_lock);
    s_game_threads[slot].alive = 0;
    s_game_threads[slot].thr = NULL;
    mutexUnlock(&s_game_threads_lock);
  }
  return ret;
}

static int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  (void)attr;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

static int pthread_setschedparam_fake(pthread_t t, int policy, const void *param) {
  (void)t; (void)policy; (void)param; return 0;
}
static int pthread_setname_np_fake(pthread_t t, const char *name) {
  (void)t; (void)name; return 0;
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int sched_yield_fake(void) { svcSleepThread(0); return 0; }

static int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

static void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

// bionic struct stat conversion for the fd variant (path variant lives in
// libc_shim as stat_fake)
struct bionic_timespec64 { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat64 {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec64 st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

static void fill_bionic_stat(const struct stat *in, void *st) {
  struct bionic_stat64 *out = st;
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino;
  out->st_mode = in->st_mode; out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size;
  out->st_blksize = in->st_blksize; out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

static int fstat_fake(int fd, void *st) {
  struct stat in;
  if (fstat(fd, &in) != 0) return -1;
  fill_bionic_stat(&in, st);
  return 0;
}
static int lstat_fake(const char *path, void *st) {
  struct stat in;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  if (stat(path, &in) != 0) return -1; // no symlinks on fatfs: lstat == stat
  fill_bionic_stat(&in, st);
  return 0;
}

// bionic statfs (arm64): report a large amount of free space so the engine's
// free-disk-space check passes.
struct bionic_statfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree;
  uint64_t f_fsid;
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static int statfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = 0x1000;
  s->f_frsize = 0x1000;
  s->f_blocks = (4ull * 1024 * 1024 * 1024) / 0x1000; // 4 GiB total
  s->f_bfree = s->f_bavail = (2ull * 1024 * 1024 * 1024) / 0x1000; // 2 GiB free
  s->f_namelen = 255;
  return 0;
}

// minimal mmap/munmap: Switch newlib has no real mmap. Anonymous maps become
// page-aligned heap; file maps read the range into heap. munmap frees it.
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
static void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)addr; (void)prot;
  void *p = memalign(0x1000, len ? len : 0x1000);
  if (!p)
    return (void *)-1; // MAP_FAILED
  if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    lseek(fd, off, SEEK_SET);
    ssize_t got = read(fd, p, len);
    (void)got;
    lseek(fd, cur, SEEK_SET);
  } else {
    memset(p, 0, len ? len : 0x1000);
  }
  return p;
}
static int munmap_fake(void *addr, size_t len) {
  (void)len;
  if (addr && addr != (void *)-1) free(addr);
  return 0;
}
static int madvise_fake(void *addr, size_t len, int adv) {
  (void)addr; (void)len; (void)adv; return 0;
}

// ---------------------------------------------------------------------------
// networking: stubbed (no multiplayer / editor debugger on this port)
// ---------------------------------------------------------------------------

static int   net_err3(long a, long b, long c) { (void)a;(void)b;(void)c; errno = ENOSYS; return -1; }
static void *net_null(void) { return NULL; }
static int   net_0(void) { return 0; }
static int   gethostname_fake(char *n, size_t l) { if (n && l) n[0] = 0; return -1; }
static int   getaddrinfo_fake(const char *a, const char *b, const void *c, void **res) {
  (void)a; (void)b; (void)c; if (res) *res = NULL; return -4; // EAI_FAIL
}
static void  freeaddrinfo_fake(void *ai) { (void)ai; }
static const char *gai_strerror_fake(int e) { (void)e; return "EAI_FAIL"; }

// process control: single-process system, none of these can work
static int proc_enosys(void) { errno = ENOSYS; return -1; }
static int getuid_fake(void) { return 0; }

// ---------------------------------------------------------------------------
// OpenSLES: real minimal implementation over audout (see audio.c)
// ---------------------------------------------------------------------------

extern uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                               uint32_t numInterfaces, const void *pInterfaceIds,
                               const uint8_t *pInterfaceRequired);
extern const void *SL_IID_ENGINE, *SL_IID_PLAY, *SL_IID_RECORD,
                  *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_ANDROIDCONFIGURATION,
                  *SL_IID_BUFFERQUEUE, *SL_IID_EFFECTSEND, *SL_IID_ENVIRONMENTALREVERB;

// ---------------------------------------------------------------------------
// zstd trace hooks (zstd itself is built into libgodot; tracing is optional
// and weakly bound -- report "no tracing")
// ---------------------------------------------------------------------------

static unsigned long long zstd_trace_begin(void) { return 0; }
static void zstd_trace_end(void) {}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

static const DynLibFunction dynlib_functions[] = {
  // --- OpenSLES (audio.c) ---
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_EFFECTSEND", (uintptr_t)&SL_IID_EFFECTSEND },
  { "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB },

  // --- bionic runtime / logging ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&stdin_fake },
  { "stdout", (uintptr_t)&stdout_fake },
  { "stderr", (uintptr_t)&stderr_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__register_atfork", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "raise", (uintptr_t)&raise },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "kill", (uintptr_t)&proc_enosys },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "gettid", (uintptr_t)&gettid_fake2 },

  // --- dynamic loader ---
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },

  // --- setjmp/longjmp ---
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "madvise", (uintptr_t)&madvise_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strncat", (uintptr_t)&strncat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strnlen", (uintptr_t)&strnlen },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "atoi", (uintptr_t)&atoi },
  { "atol", (uintptr_t)&atol },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "tolower", (uintptr_t)&tolower },
  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "lldiv", (uintptr_t)&lldiv },

  // --- ctype ---
  { "isalnum", (uintptr_t)&isalnum },
  { "islower", (uintptr_t)&islower },
  { "isupper", (uintptr_t)&isupper },
  { "isspace", (uintptr_t)&isspace },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "isdigit_l", (uintptr_t)&isdigit_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l_fake },
  { "islower_l", (uintptr_t)&islower_l_fake },
  { "isupper_l", (uintptr_t)&isupper_l_fake },
  { "tolower_l", (uintptr_t)&tolower_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },

  // --- wide char / wctype ---
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "getwc", (uintptr_t)&getwc },
  { "fputwc", (uintptr_t)&fputwc },
  { "ungetwc", (uintptr_t)&ungetwc },

  // --- locale ---
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },

  // --- printf / scanf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "vprintf", (uintptr_t)&vprintf_fake },
  { "putchar", (uintptr_t)&putchar_fake },
  { "puts", (uintptr_t)&puts_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "perror", (uintptr_t)&perror_fake },

  // --- stdio over fake __sF + buffered fopen ---
  { "fopen", (uintptr_t)&fopen_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "ftello", (uintptr_t)&ftello_fake },
  { "fgetpos", (uintptr_t)&fgetpos_fake },
  { "fsetpos", (uintptr_t)&fsetpos_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof_fake },
  { "fgetc", (uintptr_t)&getc_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "rewind", (uintptr_t)&rewind_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fcntl", (uintptr_t)&fcntl },
  { "fsync", (uintptr_t)&fsync },
  { "fstat", (uintptr_t)&fstat_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "unlink", (uintptr_t)&unlink_fake },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "rmdir", (uintptr_t)&rmdir_fake },
  { "lseek", (uintptr_t)&lseek_fake },
  { "open", (uintptr_t)&open_fake },
  { "__open_2", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake },
  { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write },
  { "access", (uintptr_t)&access_fake },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "chdir", (uintptr_t)&chdir },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "readlink", (uintptr_t)&readlink_fake },
  { "realpath", (uintptr_t)&realpath_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "closedir", (uintptr_t)&closedir_fake },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "truncate", (uintptr_t)&truncate_fake },
  { "chmod", (uintptr_t)&ret0 },
  { "fchmod", (uintptr_t)&ret0 },
  { "fchmodat", (uintptr_t)&fchmodat_fake },
  { "utimensat", (uintptr_t)&utimensat_fake },
  { "link", (uintptr_t)&proc_enosys },
  { "symlink", (uintptr_t)&proc_enosys },
  { "mkfifo", (uintptr_t)&proc_enosys },
  { "mkstemp", (uintptr_t)&mkstemp_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "isatty", (uintptr_t)&isatty_fake },
  { "ioctl", (uintptr_t)&ret0 },
  { "poll", (uintptr_t)&ret0 },
  { "select", (uintptr_t)&proc_enosys },
  { "sendfile", (uintptr_t)&proc_enosys },
  { "dup2", (uintptr_t)&proc_enosys },
  { "pipe", (uintptr_t)&proc_enosys },
  { "popen", (uintptr_t)&net_null },
  { "pclose", (uintptr_t)&proc_enosys },

  // --- math ---
  { "acos", (uintptr_t)&acos }, { "acosf", (uintptr_t)&acosf },
  { "acosh", (uintptr_t)&acosh },
  { "asin", (uintptr_t)&asin }, { "asinf", (uintptr_t)&asinf },
  { "asinh", (uintptr_t)&asinh },
  { "atan", (uintptr_t)&atan }, { "atanf", (uintptr_t)&atanf },
  { "atanh", (uintptr_t)&atanh },
  { "atan2", (uintptr_t)&atan2 }, { "atan2f", (uintptr_t)&atan2f },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "cos", (uintptr_t)&cos }, { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh },
  { "sin", (uintptr_t)&sin }, { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh }, { "sinhf", (uintptr_t)&sinhf },
  { "tan", (uintptr_t)&tan }, { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh }, { "tanhf", (uintptr_t)&tanhf },
  { "exp", (uintptr_t)&exp }, { "expf", (uintptr_t)&expf },
  { "exp2", (uintptr_t)&exp2 }, { "exp2f", (uintptr_t)&exp2f },
  { "pow", (uintptr_t)&pow }, { "powf", (uintptr_t)&powf },
  { "log", (uintptr_t)&log }, { "logf", (uintptr_t)&logf },
  { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 }, { "log2f", (uintptr_t)&log2f },
  { "log10l", (uintptr_t)&log10l_fake },
  { "hypot", (uintptr_t)&hypot }, { "hypotf", (uintptr_t)&hypotf },
  { "fmod", (uintptr_t)&fmod }, { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "ldexp", (uintptr_t)&ldexp }, { "ldexpf", (uintptr_t)&ldexpf },
  { "ilogb", (uintptr_t)&ilogb },
  { "nextafter", (uintptr_t)&nextafter },
  { "remainder", (uintptr_t)&remainder },
  { "lrintf", (uintptr_t)&lrintf },
  { "lroundf", (uintptr_t)&lroundf },
  { "modf", (uintptr_t)&modf }, { "modff", (uintptr_t)&modff },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },

  // --- fenv ---
  { "fegetenv", (uintptr_t)&fegetenv },
  { "fesetenv", (uintptr_t)&fesetenv },
  { "fesetround", (uintptr_t)&fesetround },

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime },
  { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // --- syscalls / process ---
  { "getpid", (uintptr_t)&getpid },
  { "getuid", (uintptr_t)&getuid_fake },
  { "getpwuid", (uintptr_t)&ret0 },
  { "getrlimit", (uintptr_t)&getrlimit_fake },
  { "fork", (uintptr_t)&proc_enosys },
  { "waitpid", (uintptr_t)&proc_enosys },
  { "execvp", (uintptr_t)&proc_enosys },
  { "setsid", (uintptr_t)&proc_enosys },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_gettid_np", (uintptr_t)&pthread_gettid_np_fake },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // --- POSIX semaphores (pointer-indirected; libc_shim) ---
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- scheduling ---
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_getaffinity", (uintptr_t)&sched_getaffinity_fake },
  { "sched_setaffinity", (uintptr_t)&sched_setaffinity_fake },

  // --- networking (stubbed) ---
  { "socket", (uintptr_t)&proc_enosys },
  { "bind", (uintptr_t)&net_err3 },
  { "listen", (uintptr_t)&net_err3 },
  { "accept", (uintptr_t)&net_err3 },
  { "connect", (uintptr_t)&net_err3 },
  { "send", (uintptr_t)&net_err3 },
  { "sendto", (uintptr_t)&net_err3 },
  { "recv", (uintptr_t)&net_err3 },
  { "recvfrom", (uintptr_t)&net_err3 },
  { "setsockopt", (uintptr_t)&net_err3 },
  { "getsockopt", (uintptr_t)&net_err3 },
  { "getsockname", (uintptr_t)&net_err3 },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&net_err3 },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "if_indextoname", (uintptr_t)&net_null },
  { "if_nametoindex", (uintptr_t)&net_0 },
  { "inet_pton", (uintptr_t)&net_0 },
  { "in6addr_any", (uintptr_t)&in6addr_any_fake },

  // --- zlib (host -lz) ---
  { "adler32", (uintptr_t)&adler32 },
  { "crc32", (uintptr_t)&crc32 },
  { "compress", (uintptr_t)&compress },
  { "compress2", (uintptr_t)&compress2 },
  { "compressBound", (uintptr_t)&compressBound },
  { "uncompress", (uintptr_t)&uncompress },
  { "deflate", (uintptr_t)&deflate },
  { "deflateBound", (uintptr_t)&deflateBound },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit_", (uintptr_t)&deflateInit_ },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateReset2", (uintptr_t)&inflateReset2 },

  // --- zstd trace hooks ---
  { "ZSTD_trace_compress_begin", (uintptr_t)&zstd_trace_begin },
  { "ZSTD_trace_compress_end", (uintptr_t)&zstd_trace_end },
  { "ZSTD_trace_decompress_begin", (uintptr_t)&zstd_trace_begin },
  { "ZSTD_trace_decompress_end", (uintptr_t)&zstd_trace_end },

  // --- Android NDK: asset manager (godot_shim) ---
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },

  // --- Android NDK: looper / native window ---
  { "ALooper_acquire", (uintptr_t)&ret0 },
  { "ALooper_release", (uintptr_t)&ret0 },
  { "ALooper_prepare", (uintptr_t)&ret0 },
  { "ALooper_pollOnce", (uintptr_t)&retm1 },
  { "ALooper_wake", (uintptr_t)&ret0 },
  { "ANativeWindow_acquire", (uintptr_t)&ret0 },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },

  // --- Android NDK: camera2 / media (stubbed; CameraServer stays empty) ---
  { "ACameraManager_create", (uintptr_t)&ret0 },
  { "ACameraManager_delete", (uintptr_t)&ret0 },
  { "ACameraManager_getCameraIdList", (uintptr_t)&retm1 },
  { "ACameraManager_deleteCameraIdList", (uintptr_t)&ret0 },
  { "ACameraManager_getCameraCharacteristics", (uintptr_t)&retm1 },
  { "ACameraManager_openCamera", (uintptr_t)&retm1 },
  { "ACameraMetadata_free", (uintptr_t)&ret0 },
  { "ACameraMetadata_getConstEntry", (uintptr_t)&retm1 },
  { "ACameraDevice_close", (uintptr_t)&ret0 },
  { "ACameraDevice_createCaptureRequest", (uintptr_t)&retm1 },
  { "ACameraDevice_createCaptureSession", (uintptr_t)&retm1 },
  { "ACameraCaptureSession_close", (uintptr_t)&ret0 },
  { "ACameraCaptureSession_setRepeatingRequest", (uintptr_t)&retm1 },
  { "ACameraCaptureSession_stopRepeating", (uintptr_t)&retm1 },
  { "ACameraOutputTarget_create", (uintptr_t)&retm1 },
  { "ACameraOutputTarget_free", (uintptr_t)&ret0 },
  { "ACaptureRequest_addTarget", (uintptr_t)&retm1 },
  { "ACaptureRequest_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutput_create", (uintptr_t)&retm1 },
  { "ACaptureSessionOutput_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutputContainer_create", (uintptr_t)&retm1 },
  { "ACaptureSessionOutputContainer_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutputContainer_add", (uintptr_t)&retm1 },
  { "AImageReader_new", (uintptr_t)&retm1 },
  { "AImageReader_delete", (uintptr_t)&ret0 },
  { "AImageReader_getWindow", (uintptr_t)&retm1 },
  { "AImageReader_setImageListener", (uintptr_t)&retm1 },
  { "AImageReader_acquireNextImage", (uintptr_t)&retm1 },
  { "AImage_delete", (uintptr_t)&ret0 },
  { "AImage_getPlaneData", (uintptr_t)&retm1 },
  { "AImage_getPlanePixelStride", (uintptr_t)&retm1 },
  { "AImage_getPlaneRowStride", (uintptr_t)&retm1 },

  // --- EGL (mesa; the wrapper owns the context) ---
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },

  // --- GLES3 core (mesa), generated from the .so's UND list ---
#include "gl_imports.inc"
};

static const size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void smwr_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, (DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, 1);
}

// generic import lookup for dlsym_fake (egl_shim routes unknown names here)
uintptr_t smwr_find_import(const char *name) {
  DynLibFunction *f = so_find_import((DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, name);
  return f ? f->func : 0;
}
