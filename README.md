<div align="center"><img src="./extras/Logo.png"></img></div>
<h1 align="center">Super Mario World Remastered Plus · Nintendo Switch port</h1>

A wrapper/loader that runs the **Android release of Super Mario World
Remastered Plus** (a fully open-source fan game by [BTFighter](https://github.com/BTFighter/Super-Mario-World-Remastered-Plus/releases/tag/v1.1), built on **Godot
4.4**) on the Nintendo Switch. It loads the original `arm64-v8a`
`libgodot_android.so`, provides a minimal Android-like environment (fake JNI,
a bionic→newlib libc shim, and a GLES3/EGL import table backed by mesa), and
drives the engine's `GodotLib` native lifecycle directly.

### How to install

On your SD card, the app lives in `/switch/smwr_nx/`:

```
/switch/smwr_nx/smwr.nro              <- the port (this repo builds it)
/switch/smwr_nx/libgodot_android.so   <- arm64-v8a engine binary (from the APK)
/switch/smwr_nx/libc++_shared.so      <- arm64-v8a C++ runtime (from the APK)
/switch/smwr_nx/assets/               <- game data (the APK's assets/ folder)
/switch/smwr_nx/save/baserom.sfc      <- your legally own Super Mario World dump.
```

Both `.so` files must be the **arm64-v8a** ones (`lib/arm64-v8a/` inside the
APK) — not the armeabi-v7a ones.

Launch it through a **game/title override** (hold R on a game and open it) or a
forwarder, so the port runs with full RAM. It will not work in applet/album mode.

Saves and `config.txt` live in `/switch/smwr_nx/save/`.

### Multiple players

Up to four controllers are read separately, so the game's co-op works. Each pad
is announced to the engine as its own device, hot-plugged and hot-removed while
the game runs. Pro Controllers, Joy-Con pairs and lone sideways Joy-Cons are all
detected from the npad style, and the Joy-Cons attached to the console take the
first player slot nobody else is using.

Three optional `config.txt` keys:

```
controller_menu 1   # 0 = never show the system controller-assignment applet,
                    # 1..4 = show it asking for at least that many players
split_joycons 0     # 1 = force every Joy-Con pair to split into two players
joycon_turn 3       # quarter turns clockwise applied to a lone left Joy-Con
                    # (0..3); the right one gets the mirror of this
```

### How to build

You need devkitA64 (devkitPro) with these packages:

* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-zlib`

Then, from the **devkitPro msys2 shell**:

```sh
make
```

### Credits
- <b>BTFighter</b> - Super Mario World Remastered Plus (the game, ISC license)
- <b>elliencode</b> - the original Android so-loader
- <b>fgsfds</b> and <b>NaGaa95</b> - the Switch so-loader groundwork reused here

### Legal

This project has no affiliation with Nintendo. It contains no game code or
assets — supply your own `libgodot_android.so`, `libc++_shared.so` and
`assets/` from the fan game's APK. The fan game itself is open source (ISC) at
[BTFighter/Super-Mario-World-Remastered-Plus](https://github.com/BTFighter/Super-Mario-World-Remastered-Plus)
and requires assets derived from a game you legally own. Wrapper source under
the MIT License; see LICENSE.
