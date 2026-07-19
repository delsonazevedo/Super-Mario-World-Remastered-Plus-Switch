/* imports.h -- libgodot_android.so import resolution. MIT license; see LICENSE. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

void smwr_resolve_imports(so_module *mod);

// look up a name in the wrapper's import table (0 if absent);
// used by dlsym_fake so dlopen'd modules see the same environment
uintptr_t smwr_find_import(const char *name);

// engine threads created through the pthread shim (for the hang watchdog)
#include <switch.h>
int smwr_engine_threads(Thread **out_thr, void **out_entry, int max);

#endif
