/* godot_shim.h -- bionic/NDK shims added for libgodot_android.so (Godot 4.6)
 * on top of the generic libc_shim. MIT license; see LICENSE. */

#ifndef __GODOT_SHIM_H__
#define __GODOT_SHIM_H__

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>

// --- bionic dirent (layout differs from newlib's) ---
void *opendir_fake(const char *path);
void *fdopendir_fake(int fd);
void *readdir_fake(void *dirp);
int   closedir_fake(void *dirp);

// --- pthread rwlock (bionic type is opaque-inline; pointer-indirected) ---
int pthread_rwlock_rdlock_fake(void **lk);
int pthread_rwlock_wrlock_fake(void **lk);
int pthread_rwlock_unlock_fake(void **lk);

// --- misc bionic/linux ---
int      gettid_fake2(void);
int      pthread_gettid_np_fake(void *thread);
unsigned long getauxval_fake(unsigned long type);
int      __system_property_get_fake(const char *name, char *value);
int      getrlimit_fake(int res, void *rlim);
char    *realpath_fake(const char *path, char *resolved);
int      mkstemp_fake(char *tmpl);
int      mkdir_fake(const char *path, int mode);
int      unlink_fake(const char *path);
int      rmdir_fake(const char *path);
int      rename_fake(const char *from, const char *to);
int      remove_fake(const char *path);
int      openat_fake(int dirfd, const char *path, int flags, ...);
int      unlinkat_fake(int dirfd, const char *path, int flags);
int      fchmodat_fake(int dirfd, const char *path, int mode, int flags);
int      utimensat_fake(int dirfd, const char *path, const void *times, int flags);
long     pathconf_fake(const char *path, int name);
int      sched_getaffinity_fake(int pid, size_t setsize, void *mask);
int      sched_setaffinity_fake(int pid, size_t setsize, const void *mask);
int      __cxa_thread_atexit_impl_fake(void (*dtor)(void *), void *obj, void *dso);
void     __FD_SET_chk_fake(int fd, void *set, size_t setsize);
int      statvfs_fake(const char *path, void *buf);
int      truncate_fake(const char *path, int64_t len);
int      __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va);
void     perror_fake(const char *s);
int      isatty_fake(int fd);
char    *getcwd_fake(char *buf, size_t size);
int      android_log_write_fake(int prio, const char *tag, const char *msg);

// --- locale _l variants (locale argument ignored) ---
int      strcoll_l_fake(const char *a, const char *b, void *loc);
size_t   strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc);
size_t   strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc);
int      wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc);
size_t   wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
int      towlower_l_fake(int c, void *loc);
int      towupper_l_fake(int c, void *loc);
int      iswctype_l_ret0(int c, void *loc);
int      isdigit_l_fake(int c, void *loc);
int      isxdigit_l_fake(int c, void *loc);
int      islower_l_fake(int c, void *loc);
int      isupper_l_fake(int c, void *loc);
int      tolower_l_fake(int c, void *loc);
int      toupper_l_fake(int c, void *loc);
long double log10l_fake(long double x);
int      iswalpha_l_fake(int c, void *loc);
int      iswblank_l_fake(int c, void *loc);
int      iswcntrl_l_fake(int c, void *loc);
int      iswdigit_l_fake(int c, void *loc);
int      iswlower_l_fake(int c, void *loc);
int      iswprint_l_fake(int c, void *loc);
int      iswpunct_l_fake(int c, void *loc);
int      iswspace_l_fake(int c, void *loc);
int      iswupper_l_fake(int c, void *loc);
int      iswxdigit_l_fake(int c, void *loc);

// --- AAssetManager over <data_root>/assets/ ---
void    *AAssetManager_fromJava_fake(void *env, void *assetManager);
void    *AAssetManager_open_fake(void *mgr, const char *filename, int mode);
int      AAsset_read_fake(void *asset, void *buf, size_t count);
int64_t  AAsset_seek_fake(void *asset, int64_t offset, int whence);
int64_t  AAsset_getLength_fake(void *asset);
int64_t  AAsset_getLength64_fake(void *asset);
void     AAsset_close_fake(void *asset);

// --- data symbols ---
extern unsigned char in6addr_any_fake[16];
extern FILE *stdin_fake, *stdout_fake, *stderr_fake;

#endif
