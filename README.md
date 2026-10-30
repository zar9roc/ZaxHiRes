# ZaxHiRes

High resolution patch for **Zax: The Alien Hunter** (Reflexive Entertainment, 2001).

Runs the game at any resolution your display offers (tested at 1280×800 and 1920×1080), with menus, HUD and inventory laid out for the real screen size. `Zax.exe` is not modified: ZaxHiRes is a DxWrapper plugin (`.asi`) that patches the game in memory at start-up.

**Download:** the ready-to-use zip (patch + DxWrapper) is on the [Releases](../../releases) page. Installation and troubleshooting are in its `README.txt`.

## Features

- Any resolution from 800×600 up, any aspect ratio.
- *Options > Configure Video > Resolution* becomes a drop-down listing every resolution the display reports; a new choice applies at the next start.
- Menus centred, HUD bar stretched to the screen width, inventory centred with its rock border extended to the screen edges.
- Zoom is kept between levels and no longer limited at high resolutions; fixes the camera ending up in the top-left corner after loading a save.
- 3D sound centred on the middle of the view.
- Terrain tile cache sized for the resolution: ~80–90 fps at 1920×1080 instead of ~40.

## Requirements

- One of these `Zax.exe` builds (on any other exe the plugin patches nothing and says so in `ZaxHiRes.log`):
  - 2001-08-02 (PE timestamp `0x3b68d52c`; tested exe SHA-256 `7d9efcae1ef09975b0826e7c5d164d97a490bda5fa826ddf906e2e4acea236e4`)
  - 2001-10-24, the Polish JoWooD release (PE timestamp `0x3bd755b6`; SHA-256 `a3ec536518d8551333f868eaa98e5e3c4432e26e48940abf6c044341508809f9`)
- [DxWrapper](https://github.com/elishacloud/dxwrapper) with `LoadPlugins = 1` and `Dd7to9 = 1` (the release zip includes version 1.0.6334.21, configured).
- Windows, or Linux / Steam Deck with Wine or Proton (launch option `WINEDLLOVERRIDES="ddraw=n,b" %command%`). Tested on Steam Deck with Proton.

## Usage

Pick the resolution in the game's Options menu, or pass `/ScreenWidth W /ScreenHeight H` on the command line. A choice made in the menu is stored in `Zax.CFG` (`[ZaxHiRes]`) and wins over the command line; delete that section to go back. ZaxHiRes writes nothing else: the game's own `[Options]` settings are left as they were, so removing the patch gives back the game exactly as it was.

Diagnostic switches (command line): `/LogFps`, `/LogSleep`, `/LogHud`, `/CamProbe`, `/CompStats`, `/CompVerify`. Experimental: `/FastCopy`, `/CompC`, `/CompAvx2` (no measurable gain since the tile cache fix), `/NoPoolFix` (original tile cache size, for comparison).

## Building

Needs [Zig 0.13](https://ziglang.org/download/) (used as a C cross-compiler and linker; no C runtime, links only kernel32 and user32):

```sh
ZIG_DIR=/path/to/zig-linux-x86_64-0.13.0 ./build.sh               # builds ZaxHiRes.asi
GAME_DIR=/path/to/folder/with/Zax.exe ./build.sh                 # also installs it
```

## Files

| File | |
|---|---|
| `zaxhires.c` | the plugin: patch table, hooks, display mode list, Options menu drop-down |
| `comp16.h`, `comp16_test.c` | experimental SIMD terrain compositor and its test against the scalar version |
| `perf_funcs.py` | per-function breakdown of a `perf` recording of the game |
| `NOTES.md` | reverse-engineering notes on `Zax.exe`: addresses, structures, findings |

## License

MIT, see [LICENSE](LICENSE). DxWrapper, included in the release zip, is © Elisha Riedlinger under the zlib license.

*Zax: The Alien Hunter* is © Reflexive Entertainment / JoWooD. This project contains no game files.
