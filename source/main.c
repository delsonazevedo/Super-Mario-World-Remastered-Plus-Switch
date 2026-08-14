/* main.c -- Super Mario World Remastered Plus (Godot 4.6, Android) Switch
 * wrapper entry point.
 *
 * Loads the arm64-v8a libc++_shared.so + libgodot_android.so pair, provides a
 * minimal Android-like environment (fake JNI, libc/GLES3/EGL import table),
 * owns the EGL/GLES3 context, and drives the GodotLib native lifecycle
 * (initialize/setup/newcontext/resize/step) plus joypad/touch input from the
 * Switch pad and touchscreen.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "patch.h"
#include "libc_shim.h"
#include "hotfix.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cxx_mod, game_mod;

// reserve a slice for the .so loader; the rest is the newlib heap where the
// engine's malloc lands. libgodot_android.so's LOAD zone is ~73 MB and
// libc++_shared.so's ~1.3 MB. Requires full-RAM mode (title override /
// forwarder) for the engine heap.
#define SO_HEAP_RESERVE (88 * 1024 * 1024)
#define CXX_SO_SLICE    (2 * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", SO_NAME);
  if (stat(CXX_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", CXX_SO_NAME);
  char assets[300];
  snprintf(assets, sizeof(assets), "%s/assets/project.binary", config.data_root);
  if (stat(assets, &st) < 0)
    fatal_error("Could not find\nassets/project.binary.\nCopy the APK's assets/ folder next to the NRO.");
}

// Resolve the app's data directory from the launch CWD so the port works from
// any folder under /switch (not just /switch/smwr_nx). Falls back to the
// compile-time default when the CWD doesn't hold libgodot_android.so.
static void resolve_data_root(void) {
  char cwd[256];
  if (!getcwd(cwd, sizeof(cwd)) || !cwd[0]) return;
  // drop any "device:" prefix ("sdmc:/switch/x" -> "/switch/x")
  char *colon = strchr(cwd, ':');
  char *base = colon ? colon + 1 : cwd;
  if (!base[0]) return;
  size_t l = strlen(base);
  while (l > 1 && base[l - 1] == '/') base[--l] = 0; // strip trailing slashes
  // only adopt it if the game binary is actually there
  char so[300];
  snprintf(so, sizeof(so), "%s/%s", base, SO_NAME);
  struct stat st;
  if (stat(so, &st) != 0) return;
  snprintf(config.data_root, sizeof(config.data_root), "%s", base);
  snprintf(config.save_root, sizeof(config.save_root), "%s/save", base);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES3 context (mesa). Godot's android GL path expects an external
// context that is current on the thread that calls step(), so the wrapper
// owns it, exactly like the Java GLSurfaceView does on Android.
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

static int egl_setup(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY) return -1;
  if (eglInitialize(s_dpy, NULL, NULL) == EGL_FALSE) return -2;
  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) return -3;

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg;
  EGLint num = 0;
  if (eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &num) == EGL_FALSE || num < 1)
    return -4;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)win, NULL);
  if (s_surf == EGL_NO_SURFACE) return -5;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT) return -6;
  return 0;
}

// ---------------------------------------------------------------------------
// GodotLib native entry points (platform/android/java_godot_lib_jni.h)
// ---------------------------------------------------------------------------

typedef uint8_t jboolean;

static int      (*e_JNI_OnLoad)(void *vm, void *reserved);
// Godot >= 4.5: initialize(godot, asset_mgr, io, net, dirh, fileh, expansion)
static jboolean (*e_initialize)(void *env, void *cls, void *godot, void *asset_mgr,
                                void *io, void *net_utils, void *dir_handler,
                                void *file_handler, jboolean use_apk_expansion);
// Godot <= 4.4: same but with the Activity as the first object argument
static jboolean (*e_initialize44)(void *env, void *cls, void *activity, void *godot,
                                  void *asset_mgr, void *io, void *net_utils,
                                  void *dir_handler, void *file_handler,
                                  jboolean use_apk_expansion);
static int s_glue_44 = 0; // JNI glue generation of the loaded libgodot
static void     (*e_ondestroy)(void *env, void *cls);
static jboolean (*e_setup)(void *env, void *cls, void *cmdline_array, void *tts);
static void     (*e_resize)(void *env, void *cls, void *surface, int w, int h);
static void     (*e_newcontext)(void *env, void *cls, void *surface);
static jboolean (*e_step)(void *env, void *cls);
static void     (*e_key)(void *env, void *cls, int keycode, int unicode, int label, jboolean pressed, jboolean echo);
static void     (*e_joybutton)(void *env, void *cls, int device, int button, jboolean pressed);
static void     (*e_joyaxis)(void *env, void *cls, int device, int axis, float value);
static void     (*e_joyhat)(void *env, void *cls, int device, int hat_x, int hat_y);
static void     (*e_joyconnectionchanged)(void *env, void *cls, int device, jboolean connected, void *name);
static void     (*e_dispatchTouchEvent)(void *env, void *cls, int ev, int pointer, int count, void *positions, jboolean double_tap);
static void     (*e_focusin)(void *env, void *cls);
static void     (*e_focusout)(void *env, void *cls);
static void     (*e_onRendererResumed)(void *env, void *cls);
static void     (*e_onRendererPaused)(void *env, void *cls);

#define G "Java_org_godotengine_godot_GodotLib_"

static void resolve_entry_points(void) {
  e_JNI_OnLoad           = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_initialize           = (void *)so_find_addr_rx(&game_mod, G "initialize");
  e_initialize44         = (void *)e_initialize;
  // initialize() gained/lost the Activity argument across engine versions;
  // hardwareKeyboardConnected only exists on the new-signature builds (4.5+),
  // so use it to pick the calling convention.
  s_glue_44 = so_try_find_addr_rx(&game_mod, G "hardwareKeyboardConnected") == 0;
  debugPrintf("== godot JNI glue: %s-style initialize ==\n", s_glue_44 ? "4.4" : "4.6");
  e_ondestroy            = (void *)so_try_find_addr_rx(&game_mod, G "ondestroy");
  e_setup                = (void *)so_find_addr_rx(&game_mod, G "setup");
  e_resize               = (void *)so_find_addr_rx(&game_mod, G "resize");
  e_newcontext           = (void *)so_find_addr_rx(&game_mod, G "newcontext");
  e_step                 = (void *)so_find_addr_rx(&game_mod, G "step");
  e_key                  = (void *)so_try_find_addr_rx(&game_mod, G "key");
  e_joybutton            = (void *)so_try_find_addr_rx(&game_mod, G "joybutton");
  e_joyaxis              = (void *)so_try_find_addr_rx(&game_mod, G "joyaxis");
  e_joyhat               = (void *)so_try_find_addr_rx(&game_mod, G "joyhat");
  e_joyconnectionchanged = (void *)so_try_find_addr_rx(&game_mod, G "joyconnectionchanged");
  e_dispatchTouchEvent   = (void *)so_try_find_addr_rx(&game_mod, G "dispatchTouchEvent");
  e_focusin              = (void *)so_try_find_addr_rx(&game_mod, G "focusin");
  e_focusout             = (void *)so_try_find_addr_rx(&game_mod, G "focusout");
  e_onRendererResumed    = (void *)so_try_find_addr_rx(&game_mod, G "onRendererResumed");
  e_onRendererPaused     = (void *)so_try_find_addr_rx(&game_mod, G "onRendererPaused");
  (void)e_key; (void)e_joyhat;
}

// ---------------------------------------------------------------------------
// input: Switch pad + touchscreen -> Godot JoyButton/JoyAxis/touch events.
// The android Java layer translates keycodes into Godot's own enums before
// crossing into native code, so we emit Godot indices directly.
// ---------------------------------------------------------------------------

#define GD_JOY_A 0
#define GD_JOY_B 1
#define GD_JOY_X 2
#define GD_JOY_Y 3
#define GD_JOY_BACK 4
#define GD_JOY_START 6
#define GD_JOY_LSTICK 7
#define GD_JOY_RSTICK 8
#define GD_JOY_L1 9
#define GD_JOY_R1 10
#define GD_JOY_DPAD_UP 11
#define GD_JOY_DPAD_DOWN 12
#define GD_JOY_DPAD_LEFT 13
#define GD_JOY_DPAD_RIGHT 14

#define GD_AXIS_LX 0
#define GD_AXIS_LY 1
#define GD_AXIS_RX 2
#define GD_AXIS_RY 3
#define GD_AXIS_LT 4
#define GD_AXIS_RT 5

static PadState pad;

// label mapping (Switch A -> Godot A, ...): with the game's ui_accept on
// Godot A and ui_back on Godot B this gives standard Switch menu behavior
// (A confirms, B backs out). Gameplay actions are tuned via the redirected
// input .tres resources in assets/smwr_inputs/ (jump=B, spin=A, run=X/Y).
static struct { u64 sw; int btn; } s_btnmap[] = {
  { HidNpadButton_A,      GD_JOY_A },        // east
  { HidNpadButton_B,      GD_JOY_B },        // south
  { HidNpadButton_X,      GD_JOY_X },        // north
  { HidNpadButton_Y,      GD_JOY_Y },        // west
  { HidNpadButton_L,      GD_JOY_L1 },
  { HidNpadButton_R,      GD_JOY_R1 },
  { HidNpadButton_StickL, GD_JOY_LSTICK },
  { HidNpadButton_StickR, GD_JOY_RSTICK },
  { HidNpadButton_Plus,   GD_JOY_START },
  { HidNpadButton_Minus,  GD_JOY_BACK },
  { HidNpadButton_Up,     GD_JOY_DPAD_UP },
  { HidNpadButton_Down,   GD_JOY_DPAD_DOWN },
  { HidNpadButton_Left,   GD_JOY_DPAD_LEFT },
  { HidNpadButton_Right,  GD_JOY_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;
static int s_touching = 0;
static float s_prev_axis[6] = { 99, 99, 99, 99, 99, 99 }; // force initial send

static float stick_norm(s32 v) {
  float f = v / 32767.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  return f;
}

static void send_axis(void *cls, int axis, float v) {
  if (v == s_prev_axis[axis]) return;
  s_prev_axis[axis] = v;
  if (e_joyaxis) e_joyaxis(fake_env, cls, 0, axis, v);
}

static void poll_input(void) {
  void *cls = jni_activity_class();
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  if (e_joybutton) {
    for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
      const u64 m = s_btnmap[i].sw;
      if ((cur & m) && !(s_prev_buttons & m))      e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 1);
      else if (!(cur & m) && (s_prev_buttons & m)) e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 0);
    }
  }

  // sticks: godot's android convention is Y-down-positive
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);
  send_axis(cls, GD_AXIS_LX, stick_norm(l.x));
  send_axis(cls, GD_AXIS_LY, -stick_norm(l.y));
  send_axis(cls, GD_AXIS_RX, stick_norm(r.x));
  send_axis(cls, GD_AXIS_RY, -stick_norm(r.y));
  // ZL/ZR as digital triggers
  send_axis(cls, GD_AXIS_LT, (cur & HidNpadButton_ZL) ? 1.0f : 0.0f);
  send_axis(cls, GD_AXIS_RT, (cur & HidNpadButton_ZR) ? 1.0f : 0.0f);

  s_prev_buttons = cur;

  // single-finger touch, scaled from the 1280x720 panel to the surface size
  if (e_dispatchTouchEvent) {
    HidTouchScreenState ts = {0};
    const int have = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    if (have || s_touching) {
      float x = have ? (float)ts.touches[0].x * screen_width  / 1280.0f : 0.0f;
      float y = have ? (float)ts.touches[0].y * screen_height / 720.0f : 0.0f;
      float pos[3] = { 0.0f, x, y };
      void *arr = jni_new_float_array(3, pos);
      if (have && !s_touching)      { e_dispatchTouchEvent(fake_env, cls, 0 /*DOWN*/, 0, 1, arr, 0); s_touching = 1; }
      else if (have)                { e_dispatchTouchEvent(fake_env, cls, 2 /*MOVE*/, 0, 1, arr, 0); }
      else                          { e_dispatchTouchEvent(fake_env, cls, 1 /*UP*/,   0, 1, arr, 0); s_touching = 0; }
      jni_release_local(arr);
    }
  }
}

// ---------------------------------------------------------------------------
// game thread (owns the EGL context and the whole GodotLib lifecycle)
// ---------------------------------------------------------------------------

static Thread s_game_thread;
static volatile int s_game_running = 1;
static volatile int s_focused = 1;
static volatile int s_frames_done = 0; // step() iterations completed (watchdog)

// ---------------------------------------------------------------------------
// lightweight always-on telemetry: boot phase timings + gameplay stalls,
// written to <data_root>/boot_stats.txt (one write per event; negligible cost)
// ---------------------------------------------------------------------------

static u64 s_t_boot;
static FILE *s_stats;

static void stats_open(void) {
  char p[300];
  snprintf(p, sizeof(p), "%s/boot_stats.txt", config.data_root);
  s_stats = fopen(p, "w");
  s_t_boot = armGetSystemTick();
  if (s_stats) {
    fprintf(s_stats, "build " __DATE__ " " __TIME__ "\n");
    fflush(s_stats);
  }
}

static void stats_mark(const char *what) {
  if (!s_stats) return;
  const u64 ms = armTicksToNs(armGetSystemTick() - s_t_boot) / 1000000ull;
  fprintf(s_stats, "%7llu ms  %s\n", (unsigned long long)ms, what);
  fflush(s_stats);
}

static void game_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // bionic stack canary from tpidr_el0+0x28

  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  eglSwapInterval(s_dpy, 1);

  void *cls = jni_activity_class();

  if (e_JNI_OnLoad) {
    debugPrintf(">> JNI_OnLoad...\n");
    e_JNI_OnLoad(fake_vm, NULL);
    debugPrintf(">> JNI_OnLoad ok\n");
  }

  debugPrintf(">> GodotLib.initialize...\n");
  jboolean ok;
  if (s_glue_44)
    ok = e_initialize44(fake_env, cls, jni_activity_object(),
                        jni_godot_object(), jni_assetmgr_object(),
                        jni_godot_io_object(), jni_netutils_object(),
                        jni_dirhandler_object(), jni_filehandler_object(),
                        0 /* use_apk_expansion */);
  else
    ok = e_initialize(fake_env, cls,
                      jni_godot_object(), jni_assetmgr_object(),
                      jni_godot_io_object(), jni_netutils_object(),
                      jni_dirhandler_object(), jni_filehandler_object(),
                      0 /* use_apk_expansion */);
  debugPrintf(">> GodotLib.initialize -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.initialize failed.");
  stats_mark("GodotLib.initialize");

  // force the GL compatibility renderer; redundant with the project settings
  // but immune to project.binary quirks. When <data_root>/game.pck exists
  // (tools/make_pck.py), mount it as the main pack: offset reads from one
  // file instead of per-file SD path walks (much faster boot/level loads).
  static char pck_path[300];
  snprintf(pck_path, sizeof(pck_path), "%s/game.pck", config.data_root);
  struct stat pck_st;
  const int have_pck = (stat(pck_path, &pck_st) == 0);
  debugPrintf(">> main pack: %s (%s)\n", pck_path, have_pck ? "found" : "absent, using assets/");
  stats_mark(have_pck ? "main pack: FOUND (game.pck)" : "main pack: ABSENT (assets/ dir)");

  const char *args[8];
  int nargs = 0;
  args[nargs++] = "--rendering-method";
  args[nargs++] = "gl_compatibility";
  if (have_pck) {
    args[nargs++] = "--main-pack";
    args[nargs++] = pck_path;
  }
#if DEBUG_LOG
  args[nargs++] = "--verbose";
#endif
  void *cmdline = jni_new_string_array(nargs, args);

  debugPrintf(">> GodotLib.setup...\n");
  ok = e_setup(fake_env, cls, cmdline, jni_tts_object());
  debugPrintf(">> GodotLib.setup -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.setup (Main::setup) failed.\nCheck %s.", LOG_NAME);
  stats_mark("GodotLib.setup (project+drivers)");

  debugPrintf(">> newcontext/resize (%dx%d)...\n", screen_width, screen_height);
  e_newcontext(fake_env, cls, jni_surface_object());
  e_resize(fake_env, cls, NULL, screen_width, screen_height);

  debugPrintf(">> entering step loop\n");
  int frames = 0;
  int paused = 0;
  int announced_pad = 0;
  // adaptive CPU boost: shader compilation and level loads are CPU-bound
  // stalls on mesa/nouveau. Any slow step re-arms the boost; it drops only
  // after ~10 s of smooth frames. Boot naturally keeps it armed throughout.
  int boosted = 1; // main() starts boosted for load
  int calm_frames = 0;

  while (s_game_running && !jni_quit_requested) {
    if (!s_focused) {
      if (!paused) {
        if (e_focusout) e_focusout(fake_env, cls);
        if (e_onRendererPaused) e_onRendererPaused(fake_env, cls);
        paused = 1;
      }
      svcSleepThread(16 * 1000 * 1000);
      continue;
    }
    if (paused) {
      if (e_onRendererResumed) e_onRendererResumed(fake_env, cls);
      if (e_focusin) e_focusin(fake_env, cls);
      paused = 0;
    }

    if (announced_pad) poll_input();

    if (frames < 8) debugPrintf(">> step %d begin\n", frames + 1);
    const u64 t0 = armGetSystemTick();
    e_step(fake_env, cls);
    if (frames < 8) debugPrintf(">> step %d done\n", frames + 1);
    eglSwapBuffers(s_dpy, s_surf);
    const u64 step_ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;

    if (step_ms > 50) { // stall (shader compile / load): burst the CPU
      calm_frames = 0;
      if (!boosted) { cpu_boost(1); boosted = 1; }
      if (step_ms > 100 && announced_pad && s_stats && frames < 100000) {
        fprintf(s_stats, "stall %4llu ms  frame %d\n", (unsigned long long)step_ms, frames);
        fflush(s_stats); // we already dropped frames; one tiny write is noise
      }
    } else if (!config.boost && boosted && ++calm_frames > 600) { // ~10 s smooth -> stock clocks
      cpu_boost(0);
      boosted = 0;
    }

    frames++;
    s_frames_done = frames;
    if (frames == 1) stats_mark("step 1 (engine servers up)");
    if (frames == 4) stats_mark("step 4 (game scene running)");
    if (!announced_pad && frames >= 4) {
      // engine servers are up after the first steps; announce the pad once
      if (e_joyconnectionchanged) {
        void *name = jni_new_string("Nintendo Switch Controller");
        e_joyconnectionchanged(fake_env, cls, 0, 1, name);
        jni_release_local(name);
      }
      announced_pad = 1;
      debugPrintf(">> pad announced after %d frames\n", frames);
    }
  }

  debugPrintf(">> leaving step loop (running=%d quit=%d)\n", s_game_running, jni_quit_requested);
  if (e_ondestroy) e_ondestroy(fake_env, cls);
  s_game_running = 0;
}

// ---------------------------------------------------------------------------
// hang watchdog: when the game thread stops completing steps, pause it and
// dump PC/LR plus an FP-chain backtrace so the stall site lands in the log.
// Offsets are printed relative to both loaded modules and the wrapper.
// ---------------------------------------------------------------------------

int main(void); // forward-declared: the watchdog uses it as a code anchor

static void log_code_addr(const char *tag, uint64_t a) {
  // anchor the wrapper's code region on a known function (module base symbols
  // resolve to 0 under hbl); offsets are then relative to main()
  const uint64_t wrap_base = ((uint64_t)&main) & ~0xFFFFFull;
  const uint64_t gd_base = (uint64_t)game_mod.load_virtbase;
  const uint64_t cxx_base = (uint64_t)cxx_mod.load_virtbase;
  if (a >= gd_base && a < gd_base + game_mod.load_size)
    debugPrintf("[watchdog]   %s %016llx  godot+0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - gd_base));
  else if (a >= cxx_base && a < cxx_base + cxx_mod.load_size)
    debugPrintf("[watchdog]   %s %016llx  libc+++0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - cxx_base));
  else if (a >= wrap_base && a < wrap_base + 0x800000)
    debugPrintf("[watchdog]   %s %016llx  smwr+0x%llx\n", tag, (unsigned long long)a, (unsigned long long)(a - wrap_base));
  else
    debugPrintf("[watchdog]   %s %016llx\n", tag, (unsigned long long)a);
}

static void watchdog_dump_thread(Thread *t, const char *what) {
  if (R_FAILED(threadPause(t))) {
    debugPrintf("[watchdog] could not pause %s\n", what);
    return;
  }
  ThreadContext ctx;
  Result rc = svcGetThreadContext3(&ctx, t->handle);
  if (R_SUCCEEDED(rc)) {
    debugPrintf("[watchdog] %s:\n", what);
    log_code_addr("PC", ctx.pc.x);
    log_code_addr("LR", ctx.lr);
    // walk the frame-pointer chain: [fp] = next fp, [fp+8] = return address
    uint64_t fp = ctx.fp;
    const uint64_t sp = ctx.sp;
    for (int i = 0; i < 12; i++) {
      if (fp < sp || fp > sp + (16ull << 20) || (fp & 7)) break;
      const uint64_t next = *(const uint64_t *)fp;
      const uint64_t ret = *(const uint64_t *)(fp + 8);
      if (!ret) break;
      char tag[8];
      snprintf(tag, sizeof(tag), "#%d", i);
      log_code_addr(tag, ret);
      if (next <= fp) break;
      fp = next;
    }
  } else {
    debugPrintf("[watchdog] svcGetThreadContext3(%s) failed: %08x\n", what, rc);
  }
  threadResume(t);
}

static void watchdog_dump(void) {
  debugPrintf("[watchdog] bases: main()=%p godot=%p libc++=%p\n",
              (void *)&main, game_mod.load_virtbase, cxx_mod.load_virtbase);
  watchdog_dump_thread(&s_game_thread, "game thread");

  Thread *thr[16];
  void *entry[16];
  int n = smwr_engine_threads(thr, entry, 16);
  for (int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "engine thread %d (entry godot+0x%lx)", i,
             (unsigned long)((uintptr_t)entry[i] - (uintptr_t)game_mod.load_virtbase));
    watchdog_dump_thread(thr[i], what);
  }
}

static void load_module(so_module *mod, const char *name, void *base, size_t limit) {
  int res = so_load(mod, name, base, limit);
  if (res < 0)
    fatal_error("Could not load\n%s (%d).", name, res);
  debugPrintf("== so_load %s ok (load_size=%u KB) ==\n", name, (unsigned)(mod->load_size >> 10));
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  resolve_data_root(); // adopt the actual launch folder as the data root
  stats_open();
  check_data();
  apply_asset_hotfixes(); // restore game data files known to be missing from the APK export
  mkdir(config.save_root, 0777);
  {
    char cache[300];
    snprintf(cache, sizeof(cache), "%s/cache", config.save_root);
    mkdir(cache, 0777);
  }
  setenv("HOME", config.save_root, 1);

  set_screen_size(config.screen_width, config.screen_height);

  if (egl_setup() != 0)
    fatal_error("Could not create the EGL/GLES3 context.");

  debugPrintf("== SMWR+ Switch wrapper booting; build " __DATE__ " " __TIME__ "; data_root=%s ==\n", config.data_root);
  debugPrintf("== EGL/GLES3 context created (%dx%d) ==\n", screen_width, screen_height);

  // libc++ first so libgodot's C++ ABI imports resolve against it
  load_module(&cxx_mod, CXX_SO_NAME, heap_so_base, CXX_SO_SLICE);
  void *game_base = (char *)heap_so_base + CXX_SO_SLICE;
  load_module(&game_mod, SO_NAME, game_base, heap_so_limit - CXX_SO_SLICE);

  smwr_resolve_imports(&cxx_mod);
  smwr_resolve_imports(&game_mod);
  debugPrintf("== imports resolved ==\n");
  so_patch(&game_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();

  so_finalize(&cxx_mod);
  so_flush_caches(&cxx_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  debugPrintf("== so_finalize ok; running init arrays ==\n");

  jni_init();
  tls_setup_guard();
  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cxx_mod);
  so_free_temp(&game_mod);
  debugPrintf("== init arrays done ==\n");
  stats_mark("modules loaded + init arrays");

  // the game sees cwd="/" (getcwd_fake) and stray absolute writes are rebased
  // into save_root (sandbox_path); move the REAL cwd there too so any genuine
  // relative libc paths agree. The .so files were already loaded above.
  if (chdir(config.save_root) != 0)
    debugPrintf("!! chdir(%s) failed\n", config.save_root);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  if (R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2)))
    fatal_error("Could not create the game thread.");
  threadStart(&s_game_thread);

  int last_frames = -1;
  int stall_ms = 0;
  while (appletMainLoop() && s_game_running) {
    AppletFocusState fs = appletGetFocusState();
    s_focused = (fs == AppletFocusState_InFocus);

    // hang watchdog: dump the game thread's stack once every 20 s of stall
    if (s_focused) {
      if (s_frames_done != last_frames) {
        last_frames = s_frames_done;
        stall_ms = 0;
      } else if ((stall_ms += 16) >= 45000) {
        debugPrintf("[watchdog] no step completed for 45 s (steps done: %d)\n", s_frames_done);
        watchdog_dump();
        stall_ms = 0;
      }
    }
    svcSleepThread(16 * 1000 * 1000);
  }

  s_game_running = 0;
  threadWaitForExit(&s_game_thread);
  threadClose(&s_game_thread);

  if (s_ctx != EGL_NO_CONTEXT) {
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);
  }

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
