/* patch.c -- game-specific patches for libgodot_android.so (SMWR+ Final APK,
 * Godot 4.6.dev4 arm64-v8a). Offsets are for that exact binary and are
 * validated against the original instruction bytes before patching.
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <string.h>

#include "patch.h"
#include "so_util.h"
#include "util.h"

// GodotJavaViewWrapper method-id getters, called on the result of
// GodotJavaWrapper::get_godot_view() WITHOUT a null check by
// DisplayServerAndroid (mouse mode / cursor shape setup during setup2).
// If the view is null this is a Data Abort at 0x28 (the first crash seen on
// hardware). Replace with null-safe versions.

// bool GodotJavaViewWrapper::can_capture_pointer() const
// original @ 0x126c364: ldp x8, x9, [x0, #40]; ccmp...; cset; ret
#define CAN_CAPTURE_POINTER_VADDR 0x126c364
#define CAN_CAPTURE_POINTER_WORD0 0xa942a408u

static int can_capture_pointer_safe(void *self) {
  if (!self) return 0;
  void *req = *(void **)((char *)self + 0x28);
  void *rel = *(void **)((char *)self + 0x30);
  return (req && rel) ? 1 : 0;
}

typedef struct { uint32_t vaddr_word0; uint32_t expect; uintptr_t vaddr; void *repl; const char *name; } GamePatch;

void so_patch(so_module *mod) {
  static const GamePatch patches[] = {
    { 0, CAN_CAPTURE_POINTER_WORD0, CAN_CAPTURE_POINTER_VADDR,
      (void *)&can_capture_pointer_safe, "can_capture_pointer" },
  };

  for (unsigned i = 0; i < sizeof(patches) / sizeof(*patches); i++) {
    const GamePatch *p = &patches[i];
    // hooks are written into the RW backing (load_base) before so_finalize
    uint32_t *insn = (uint32_t *)((uintptr_t)mod->load_base + p->vaddr);
    if (*insn != p->expect) {
      debugPrintf("[patch] %s: unexpected bytes %08x at 0x%lx, skipping\n",
                  p->name, *insn, (unsigned long)p->vaddr);
      continue;
    }
    hook_arm64((uintptr_t)insn, (uintptr_t)p->repl);
    debugPrintf("[patch] %s hooked at vaddr 0x%lx\n", p->name, (unsigned long)p->vaddr);
  }
}
