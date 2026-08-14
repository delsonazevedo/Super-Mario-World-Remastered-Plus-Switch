/* hotfix.h -- self-healing fixes for game data known to be missing/broken in
 * the shipped APK assets. Runs once at boot, before the engine starts.
 * MIT license; see LICENSE. */

#ifndef __HOTFIX_H__
#define __HOTFIX_H__

// Writes any known-missing asset files that don't already exist under
// <data_root>/assets/. Idempotent and silent on files that are already
// present (never overwrites a user/APK-provided file). Safe to call every
// boot. See hotfix_credits.h for the specific bug this addresses.
void apply_asset_hotfixes(void);

#endif
