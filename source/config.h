/* config.h -- NBA Jam Switch wrapper configuration.
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// libgodot_android.so is fopen()'d relative to the NRO's directory (the homebrew CWD),
// so keep it a bare filename: place the NRO next to it in /switch/smwr_nx/.
#define SO_NAME "libgodot_android.so"
#define CXX_SO_NAME "libc++_shared.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "smwr_debug.log"

// '/'-absolute paths resolved against the default sdmc device. DATA_ROOT holds
// the app tree the user prepared under release/switch/smwr_nx/ (libgodot_android.so,
// libNimble.so, main.obb, assets/). SAVE_ROOT holds saves/config.
// Overridable from config.txt.
#define DEFAULT_DATA_ROOT "/switch/smwr_nx"
#define DEFAULT_SAVE_ROOT "/switch/smwr_nx/save"

// absolute so the log lands in the app dir regardless of the launch CWD
#define LOG_PATH DEFAULT_DATA_ROOT "/smwr_debug.log"

// Master debug switch: log file (<data_root>/smwr_debug.log), nxlink stdout,
// and all debugPrintf/[io]/[audio] output. On during bring-up; off for release.
#define DEBUG_LOG 0
// Per-file-operation logging (open/stat/access/fopen). Very noisy and slow
// (one fflush per line during asset loading); requires DEBUG_LOG too.
#define VERBOSE_IO 0

extern int screen_width;
extern int screen_height;

// locale reported to the engine via GodotIO.getLocale (the game is English)
#define DEVICE_LOCALE "en_US"

typedef struct {
  int screen_width;   // -1 = auto (1080p docked / 720p handheld)
  int screen_height;
  int boost;          // 0 = adaptive CPU boost (default); 1 = always boosted
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
