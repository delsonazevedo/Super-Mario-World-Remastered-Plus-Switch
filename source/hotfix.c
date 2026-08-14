/* hotfix.c -- self-healing fixes for game data known to be missing/broken in
 * the shipped APK assets. MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "hotfix.h"
#include "hotfix_credits.h"
#include "util.h"

// writes `content` (length `len`, excluding the NUL terminator) to
// <data_root>/assets/<rel_path>, but only if that file doesn't already exist.
// Returns 1 if the file was written, 0 if it was already present or on error.
static int write_if_missing(const char *rel_path, const char *content, size_t len) {
  char path[512];
  snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, rel_path);

  struct stat st;
  if (stat(path, &st) == 0) {
    return 0; // already there (APK/user-provided) -- never overwrite
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    debugPrintf("!! hotfix: could not create %s\n", path);
    return 0;
  }
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  if (written != len) {
    debugPrintf("!! hotfix: short write on %s (%zu/%zu bytes)\n", path, written, len);
    return 0;
  }
  debugPrintf(">> hotfix: wrote missing %s (%zu bytes)\n", rel_path, len);
  return 1;
}

void apply_asset_hotfixes(void) {
  // res://credits.txt: see hotfix_credits.h. Missing from Android exports of
  // this game lineage; staff_credits.gd dereferences the FileAccess result
  // with no null check, crashing the process right after the post-Bowser
  // fireworks scene (loses the ending achievement / 100% completion).
  write_if_missing("credits.txt", HOTFIX_CREDITS_TXT, sizeof(HOTFIX_CREDITS_TXT) - 1);
}
