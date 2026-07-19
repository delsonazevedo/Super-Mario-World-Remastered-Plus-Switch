/* patch.h -- game-specific patches applied to libgodot_android.so. MIT license. */

#ifndef __PATCH_H__
#define __PATCH_H__

#include "so_util.h"

// Install symbol-based hooks into the loaded module. Must run after
// so_relocate() and before so_finalize() (hook_arm64 writes into load_base).
void so_patch(so_module *mod);

#endif
