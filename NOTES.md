# Zax.exe reverse-engineering notes

Target: `Zax.exe`, PE timestamp `0x3b68d52c` (2001-08-02), image base `0x400000`, 32-bit, no ASLR.
File offset = VA - 0x400000 for `.text` (VMA 0x401000 at file offset 0x1000).
`.rdata` VMA 0x5ea000 at file 0x1ea000; `.data` VMA 0x609000 at file 0x209000.
Free code cave: 912 zero bytes at `0x5e9c70`..`0x5ea000` (end of `.text`, executable).

Disassembly used: `objdump -d -M intel --no-show-raw-insn Zax.exe` (regenerate; not kept).

## Runtime stack
`Zax.exe` -> `DDRAW.dll` (DxWrapper stub) -> `dxwrapper.dll` (Dd7to9: DirectDraw v1 -> D3D9) -> Wine d3d9/DXVK.
Needs `WINEDLLOVERRIDES="d3d8,ddraw=n,b"` (set in the Steam shortcut). DxWrapper loads `*.asi` with `LoadPlugins = 1`.
Imports: DDRAW (`DirectDrawCreate` only), DSOUND, WINMM (joy*), WSOCK32, MSACM32, binkw32. No Direct3D, software rendering.

## Display / config
| Address | What |
|---|---|
| `0x6c02cc` | global display manager; `[+4]` = display object |
| display obj vtable `+0xb4` / `+0x9c` | current screen width / height |
| display obj `+0x4c` | mode list count (max 10); entries at `+0x54 + i*12` = (width, height, bpp) |
| `0x567d60` | EnumDisplayModes callback: bpp must match; aspect (w/h) in (1.2333, 1.4333) (floats `0x5ff470`, `0x5ff46c`); size exactly 640x480 or 800x600 |
| `0x567dab` / `0x567dc0` | aspect lower / upper bound jumps (6 bytes each) |
| `0x567dd6` / `0x567de2` | `cmp edx,800` / `cmp esi,600` (imm32 at +2) |
| `0x567cd0` | find requested mode in list: exact match, else next larger, else 0x0 -> "Couldn't set resolution" and exit |
| `0x567f20` | set display mode (enumerates, sorts, SetDisplayMode via vtable `+0x54`) |
| `0x590ce0` | reads `Screen Width`/`Height` from Zax.CFG (default 640x480 from `0x4cdc70`/`0x4cdc80`), writes them back, THEN applies command line overrides (not persisted) |
| `0x4d2220` | command-line int parser: finds `/ScreenWidth`, `/ScreenHeight`, `/ScreenBitDepth`; skips `\t \r\n` (`0x622df4`), optional quote, atoi |
| `0x590f00` | forces 640x480 (some path) |
| `0x591846` region | options dialog resolution apply; `"%d X %d"` list text at `0x5919a5` |

### Options menu resolution (2026-10-28)
| Address | What |
|---|---|
| `0x567d60` | EnumDisplayModes callback (stdcall, `ret 8`): `DDSURFACEDESC` height `+0x08`, width `+0x0c`, bpp `+0x54`, green mask `+0x5c`; context = &wanted bpp. Appends (w, h, format) to the display object's list while count < 10; format 0x10 = 565, 0x0f = 555, else bpp |
| `0x567f20` | set mode: sets count `+0x4c` = 0 (`0x568064`), enumerates twice (`0x568077`, `0x56809b`), dedupes (`0x567e90`) |
| `0x590ce0` | start-up config: CFG `Screen Width`/`Height` (written back at once), then `/ScreenWidth` `0x590d73` and `/ScreenHeight` `0x590d81` (`call 0x4d2220`, name in ecx, default in edx) override them; explicit size args (movies) bypass both |
| `0x5913c2..0x591438` | Options dialog ctor: `+0x170` = mode count, `+0x174` = `+0x178` = index of the current mode; slider `+0x14c` range = count (`0x410880`), label `+0x150` |
| `0x591970` | slider moved: `+0x178` = value, label = `"%d X %d"` of that entry |
| `0x591750` | Options dialog destructor: colour depth changed -> message window (`0x417710(0x11c)` alloc, `0x403b30(title, text, 0)` thiscall, `0x40c6e0(child, 6)` on the dialog) "...will not take effect until next time the game is restarted." (strings `0x623768`, `0x623780`); resolution index changed (`0x5917cb`) -> set the mode at once (`0x567f20`) and write `Screen Width`/`Height` to the CFG |
| `0x4a4670` / `0x4a4690` | CFG read/write: `GetPrivateProfileIntA` / `WritePrivateProfileStringA` straight on the file (no in-memory copy) |

ZaxHiRes: callback `0x567d60` replaced (all modes at the colour depth: 640x480 or >= 800x600; above 10, keeps the current, the largest and 640x480, then the largest's aspect ratio and larger sizes first; ascending). `0x5917d3` -> `res_chosen`: writes `[Options]` and `[ZaxHiRes] Screen Width/Height`, shows the restart message, skips the live mode set. At start, `[ZaxHiRes]` wins over the command line if `EnumDisplaySettings` still reports that mode; the two command-line reads become `mov eax, W/H`. Delete `[ZaxHiRes]` from Zax.CFG to go back to command-line control.

Drop-down (the Colors selector's class): ctor `0x408cd0(parent, string list, rows)` (thiscall, `ret 0xc`, object 0x118 bytes, vtable `0x5eb058`; builds a popup `CJListBox` from the list and an arrow button `0x40f9b0`), select `0x408bc0(id, 0)`, selection = vtable `+0x130` (the slider's value was `+0x134`); notifies the dialog (`0x5918c0`) with code 1. String list: 0x14 bytes, ctor `0x592200` then vtable `0x5f0240` (as `0x590cbf`), append `0x54f470(const char *)` (copies), count `+0xc`. Window flag `+0x61` is generic (base window `0x40a8a0`). ZaxHiRes replaces the slider: `0x59142d` -> creates the drop-down with every collected mode (sorted; `+0x170/+0x174/+0x178` = count / pending / pending) and re-enters at `0x591459` (layout, flag, width, label); `0x5914f5` -> selects the entry, clears the label, resumes `0x591530`; `0x59197c` reads `+0x130`; `0x591988` jumps over the label update to `0x5919e2`; `res_chosen` indexes the drop-down's list.
Drop-down popup: the drop-down's `+0x100` list is the closed box's own display (kept at the box width by the resize handler `0x4082d0`). The open list is a separate popup made on every click (`0x408390` -> ctor `0x407a50(parent, drop-down, items)`, 0x25c bytes, at `+0x104`): a list box (`0x404520`) sized to the longest entry + margins (`0x404628`), at most 14 rows (`0x407a97`), then `0x407660` -> `0x4074f0` makes the scroll bar (`+0x248`, vtable `+0x180`) and takes its width out of the list's (`0x40756a`), cutting entries short (no game drop-down has enough entries to scroll). ZaxHiRes: call at `0x407af8` -> `popup_fit`, which runs `0x407660` and adds the bar's width back.

## GUI / layout engine
Window object: vtable at `+0`, rect at `+0x04` (left, top, right, bottom), parent at `+0xe8`, first child `+0xec`/`+0xf0`, sibling `+0xf4`/`+0xf8`.

| Address | What |
|---|---|
| `0x40c8d0` | AddChild(child, mode, a, x, y, flags) -> child vtable `+0xf8` (layout) if mode != 0 |
| `0x40db20` | layout method; mode jump table at `0x40e4ac` (23 entries). Mode 5 = parent origin (vtable `+0xf0` y, `+0xf4` x; both `0x4011e0` = return 0) + (x, y). Mode 6 = centre in parent (uses parent `+0x14..+0x20`) |
| `0x40df53` | mode 5 handler; ZaxHiRes hooks `0x40df61..0x40df6a` (edi = x offset, ebp = y offset, ebx = y result) |
| `0x40d630` | MoveTo(x, y) |
| `0x40d510` | SetSize(w, h) |
| `0x40f5b0` / `0x40f5d0` | SetWidth / SetHeight |
| `0x4ab5a0` | create surface (w, h, ...) |
| `0x5be4a0` | menu screen ctor (vtable `0x6055d4`): Background child (`"Background"` `0x624d14`, vtable `0x6054a4`), 640x480 panel at (80,60) mode 5, screen SetSize 800x600 at `0x5be8b9` |
| `0x5be565`, `0x5beaf6`, `0x5bec84`, `0x5beef3`, `0x5bf1c6`, `0x5bf301`, `0x429581`, `0x4f3d61`, `0x505288`, `0x5059b1` | `width == 640` special cases (else falls to the 800 layout) |
| `0x5be5b0`, `0x5be632`, `0x59de0a`, `0x59ed43` | `width == 800` checks (skip resize otherwise) |
| `0x5beae0` | page/canvas setup: 640 -> 640x480 at (80,60); else 800x600 at (0,0) (`0x5beb31`), buffer 800x600 (`0x5beb83`) |
| `0x5bede0` | another page; MoveTo(80,60) if 640 else (0,0) at `0x5beef3` |
| `0x5bec9d` | coordinate maths with 320/240 in the non-640 path (left alone; menu clicks work) |

Design: menus are an 800x600 canvas; on 640x480 the game shows its centre via (80,60) offsets.

Mode 5 parents seen at 1280x800 (from ZaxHiRes.log, first placement per class):

| vtable | Rect at first child placement | Notes |
|---|---|---|
| `0x6055d4` | 0,0 677x11, top-level | main menu screen: children are added **before** SetSize, so size checks can't identify it. Centred by vtable allowlist |
| `0x5eb404` | 80,60 640x480, parent = main menu | the 640x480 panel; its children are relative to it, so it must not be shifted again |
| `0x5f13a0` | 362,177 556x331, top-level | dialog/submenu; the game already centres it |
| `0x5ee340` | 0,0 721x200, top-level | unknown |
| `0x5eb2d4` | 0,0 0x0, top-level | unknown |

Dialogs are top-level too, so "no parent" doesn't identify menu screens either.

| Address | What |
|---|---|
| `0x5be7e4` | version label (`[screen+0x154]`, text from `0x623890`) added with mode 4 (anchor to parent bottom-right, -1000 = auto margin) |
| `0x40ddef` | mode 4 handler: parent right/bottom minus child size minus offsets |
| `0x5bf2ec` | moves the version label to (800 - w, 600 - h): `mov esi,800` / `mov ebx,600`, or 720x540 on 640. ZaxHiRes sets W/H |

## Movies
| Address | What |
|---|---|
| `0x4cd5a0` | play movie by name (`"Movies/%s.bik"` `0x615598`); returns at once if `/SkipMovies` (`0x6155a8`) is on the command line; switches to 640x480 for playback (`0x590ce0`) |
| `0x4cd780` | second movie player (same `/SkipMovies` check at `0x4cd9a2`), called from `0x4f07aa` (startup logos) |
| `0x5bfc03` | plays `"Intro"` (`0x6276c4`) |

`DATA.dat` contains only `Movies/Intro.bik`, `Movies/JoWooD.bik`, `Movies/Reflexive.bik`, so `/SkipMovies` is a skip-intro switch.

## Game view / camera

Object model (2026-10-28): `game+0x2bc` is an array of **entities** (count at `+0x2c0`), not cameras. The view state object is `game->vfunc_b0()->vfunc_5c(i)`: zoom `+0xd8`, zoom flags `+0xe0`, index of the player entity in that array `+0xdc` (set at `0x4f47a9`). Entity position = `+0x80/+0x84` floats, so the sound listener is player + (320, 240).

Camera (found 2026-10-28), all floats in the view state:

| Field | Meaning |
|---|---|
| `+0xc0/+0xc4` | camera top-left (world) |
| `+0xc8/+0xcc` | target centre |
| `+0xd0/+0xd4` | smoothed centre (eases toward target in mode 1) |
| `+0xd8` | zoom = world px per screen px (min 320/W, max min(960/W, 1); reset = 1.0) |

| Address | What |
|---|---|
| `0x4f5dd0` | SetCamera(cx, cy, mode, player); player -1 = all views. Top-left = centre - half the **world surface** size, then map clamp: mode 1/2 `0x4ed6e0` (map size `[game+0x2e0]+0x10/+0x14`), mode 0 `0x4ed7f0`; then `0x4ed8f0(player, cam)` |
| `0x4f6770` | SetZoom(zoom, player): clamps zoom so play rect * zoom fits the map (`level+0x2cfc/+0x2d00`, rounded down to 128); returns early if zoom unchanged; else centre = cam + surface/2, resize world surface `[game+0x2a4]` to play rect * zoom (`0x4aa550`), SetCamera(centre) |
| `0x4f2430` | level start: creates world surface = play rect * zoom (`0x4ab5a0`), no map-fit clamp |
| `0x4f6980` | play rect setter, also resizes the world surface (`0x4f6a73`) |

Listener update `0x4f2620` (single caller `0x59d242`) runs per frame; ZaxHiRes `/CamProbe` hooks `0x4f26e8` there and writes `ZaxCamProbe.log`: zoom, camera, target, smoothed centre, player, play rect, map size (floats * 100) and the first dwords of the world surface object, every 30 calls.

Root cause (probe runs 2026-10-28, 1920x1080):
- Entering the mothership from the overworld: SetZoom's map-fit clamp sets zoom 0.46 and surface 896x477 (consistent, centred). Level start `0x4f2551` and spawn `0x4f4810` re-apply the *current* zoom, so 0.46 sticks in every later level.
- Loading a save in the mothership: surface ends up 896x477 but zoom stays 1.0. SetZoom returns early when the zoom is unchanged, so the surface is never fixed. Camera centres on 896x477 while the renderer draws 1920x1024 at zoom 1.0 -> player at (463,246), view runs on to the bottom-right. This is the "camera in the top-left" bug.
- The "player" entity at `game+0x2bc[view+0xdc]` is the view's dolly: its `+0x80/+0x84` equals the camera top-left; its level is `+0x7c` (map size `level+0x2cfc/+0x2d00`). `0x4f10a0(i)` returns view i's dolly.
- World surface object: width in the high word of `+0x10`, height at `+0x14`.
- `game+0x2e0` `+0x10/+0x14` are floats 30.0 x 15.0 and don't change per level: not the map size.

Fix in ZaxHiRes: SetZoom entry hook remembers zoom from the zoom keys/reset/detent (return addresses `0x4f328c`, `0x4f32eb`, `0x4f3305`, `0x544a7e`, `0x544ac1`, `0x544b01`; detent `0x4f3400` only when it snaps to 1.0, because it calls SetZoom every frame with the current, possibly map-clamped zoom); per-frame hook (`0x4f26e8`) forces SetZoom(chosen zoom) on level change, or once per level when the surface doesn't match play rect * zoom (clears view zoom first so SetZoom doesn't return early). Max zoom limit forced to 1.0 (`0x4f253d`), was 960/W.
| Address | What |
|---|---|
| `0x4f6980` | play rect `this+0x2e4..0x2f0` = (0, 0, W, H-56) (56 px HUD bar); single caller `0x4f24e1` with null rect |
| `0x4f24fd` / `0x4f2520` | zoom limits: `0x618034` = 320/W (min), `0x618038` = 960/W capped at 1.0 (max) |
| `0x4f3205` | zoom in/out/reset handling (flags `+0xe0`, zoom `+0xd8`, step `0x5f1e48`) |
| `0x4f6770` | SetZoom(zoom, player), clamps so map fits play rect |
| `0x4f270c` | sound listener = view `[+0x80], [+0x84]` + (320, 240) (`0x5eff98`, `0x5eff9c`), z = -10; setter `0x57c170` (`0x7136a0`) |
| `0x470a6a` | aim cursor recentred at (320,240) +- 200 (`0x5effa0`) |
| `0x4f10a0` | get local player/view object; `[esi+0x2bc][idx]`, idx at `+0xdc` |

Ruled out for camera: all float/int 320/240/400/300/640/480/800/600 references; `0x526da0` (angle interpolation); `0x5a6730` (+-320 box around player); `0x4318b1` (debug draw); `0x4a5f2f` (font metrics); `0x430619` (+0x80 writer, not camera); `0x562e54` (alloc size 600).
Suggested next step: runtime - scanmem on the Wine process for the camera X/Y, then a winedbg hardware breakpoint on writes.

## Misc
- `Zax.CFG`: INI format, `[Options]`, CRLF.
- Debug strings: `Debug/MiniMap Y Offset` etc. in `.data` (~`0x217640`).
- Strings `"Couldn't set resolution"` `0x623830`, `"&Resolution:"` `0x62371c`, `"Screen Width"` `0x623878`, `"Screen Height"` `0x623868`.

## Performance profile (2026-10-28, 1920x1080, overworld)
`perf record -e cycles:u -F 2000 -p <Zax.exe pid> -- sleep 20`, then `perf script -F ip,dso` and bin by function start (call targets). One thread at ~97% of a core; 94.5% of samples in Zax.exe, DxWrapper/DXVK/driver < 1%.

| Function | Share | What |
|---|---|---|
| `0x541250` | 19.6% | terrain compositor: per pixel reads 8-bit layer index, copies RGB565 from layer table `+0x64`, or blends with overlay `+0x464` using weight table `+0xc64` (scalar 5/6/5 unpack) |
| `0x437890` | 10.3% | memcpy of 16-bit pixels (ecx = dst, edx = src, [esp+4] = pixel count, `ret 0x10`), dword loop |
| `0x43a6b0`, `0x43a5c0` | 12.8% | RLE sprite blitters (0x80 = skip run, 0x40 = run, else literal) with clipping |
| `0x4e8bd0` | 8.2% | entity list walk (calls `0x566e40`, checks flags), game logic |
| `0x49fad0` | 7.7% | 2x2 box filter / average on an 8-bit map (sum of 4 bytes + 2 >> 2) |
| `0x4388f0`, `0x49c890`, `0x46d900`, `0x4484c0` | ~11% | not inspected yet |

No MMX/SSE anywhere in the hot code.

### Second profile: IPC and misses (2026-10-28, 1920x1080, standing still, ~40 fps)
Recorded with per-event periods (see `perf_funcs.py`; a plain `-c` applies to every event and the first try sampled everything every 2000 events, which distorted the run). Totals: Zax.exe 87% of cycles, plugin 7.9% (shows as `[JIT] tid`, the fast row copy), dsound 2%. Overall IPC 2.26: the game is compute-bound, not memory-bound.

| Function | % of Zax.exe cycles | IPC | cache miss/ki | branch miss/ki | What |
|---|---|---|---|---|---|
| `0x541250` | 28.0 | 3.28 | 0.46 | 0.42 | terrain compositor, pure scalar compute: SIMD target |
| `0x43a6b0` | 9.9 | 1.87 | 1.74 | 6.91 | RLE sprite blitter |
| `0x43a5c0` | 6.2 | 1.34 | 3.26 | 11.27 | RLE sprite blitter |
| `0x4513b0`, `0x451310`, `0x4514f0`, `0x448e70`, `0x4516e0`, `0x449aa0`, `0x451810`, `0x4519d0`, `0x451c40` | ~11 together | 1.7-2.5 | ~1.3 | ~7 | per-run span helpers called from `0x43a6b0` (blend/tint variants?) |
| `0x49fad0` | 6.2 | 3.60 | 0.07 | 0.72 | 2x2 box filter, pure compute: SIMD target |
| `0x4e8bd0`, `0x49c890`, `0x46d900`, `0x4aa520`, `0x4b2cd0`, `0x566e40` | ~17 | 1.8 | ~3.3 | ~3.5 | entity/game logic |
| `0x4388f0` | 3.5 | 1.68 | 2.58 | 6.95 | called from `0x4a8f10` |
| `0x434a10` | 0.6 | 0.43 | 18.5 | 1.8 | caller of the row copy; memory-bound |

Sprite path total (blitters + span helpers) ~27% of Zax.exe. Compositor is called from `0x4a8820`.

Fast row copy (`rep movs`): 10.3% -> 7.9% of cycles, no measurable fps change (39-40 both): the copy is memory-bandwidth bound (1920x1024x2 = 4 MB, the size of L3). Now opt-in with `/FastCopy`.

## Terrain compositor replacement (`0x541250`, 16-bit path)
thiscall, this = terrain object; at `0x54148d` the frame holds: `+0x14` 128-entry source row offset table (pixels), `+0x40` dst surface, `+0x44` 8-bit layer mask surface, `+0x48` source x (& 0x7f), `+0x4c` row phase, `+0x50` width, `+0x54` height. Surface vtable `+0xb8` = row offset table (pixels), `+0x7c` = base address, `+0x74` = bpp. Per pixel: idx = mask byte; base texture `this+0x64[idx]`, overlay `this+0x464[idx]` (0 = none), weight `this+0xc64[idx]`; byte offset into both = (rowtab[(y + phase) & 0x7f] + sx + x) * 2.

Blend of overlay c over base d (exact, mod 2^32): `g = ((Gc - Gd) * (w>>2) >> 1) + (d & 0xffe0)`, `r = ((Rc - Rd) * (w>>3) << 6) + (d & 0xf83f)`, `b = (Bc - Bd) * (w>>3) >> 5` (logical shifts), result `b + (g & 0x7e0) + (r & 0xf800) + (d & 0x1f)`. In 16-bit SIMD lanes this is exact if blue uses an arithmetic shift and |product| < 2^15 (w <= 2047).

Code: `comp16.h` (scalar `row_c`, AVX2 `row_avx2` for 16 same-index pixels), `comp16_test.c` (AVX2 vs scalar, 40000 random rows, 0 mismatches; a deliberately wrong shift gives ~1M). Plugin switches: `/CompVerify` (original runs, ours computed into a buffer and compared; hooks `0x54148d` and exit `0x54160b`), `/CompC`, `/CompAvx2` (replace: hook `0x54148d` -> comp16, jump to `0x54160b`).

AVX2 result 1 (2026-10-28): no fps gain. Profile with `/CompAvx2`: `row_avx2` 25% of cycles (original was 28%), hot instructions are the scalar fallback, so 16-pixel groups with a single layer index are rare. Hypothesis: the index encodes the blend amount (many indices share base/overlay textures, differ in weight). `/CompStats` logs the layer tables once and classifies 16-pixel groups (same index / same textures / mixed).

Layer tables (CompStats, overworld terrain object): bands of 26 indices, each band one (base, overlay) texture pair with weights rising by 10 (A/B 9..249, B/C 3..253, C/D 7..., chained), plus base-only entries (overlay 0, weight 0); 11 pairs, all weights <= 255; textures 128x128 16-bit, 0x8008 bytes apart. 16-pixel groups: 0.4% one index, 68% one texture pair (weights differ), 32% crossing a band edge.

AVX2 v2 (`comp16.h`): per call `c16_prepare` builds pair id / pair textures / w>>2, w>>3 per index (base-only indices join a pair on the same base: weight 0 gives exactly the base pixel); per group, 16 lookups, then one blend per distinct pair, lanes merged by `cmpeq` on the pair ids. Offline (`comp16_test.c`, 120000 rows incl. game layout and free base/overlay combinations): 0 mismatches; three deliberate bugs all caught. Benchmark on a game-like 64x64 tile: scalar 4.18 ns/pixel, AVX2 3.36 (v1 per-lane loops) -> 1.15 (v2).

## Terrain tile cache (the real cost of the compositor at high resolutions)
AVX2 v2 gave no fps gain either (39-40), so the cost is elsewhere. Call chain: `0x540acc` per tile -> `0x540790` cache lookup (entry handle empty -> allocate from pool `0x7110e0` via `0x4d2680` = miss, else lock `0x4d2760` = hit) -> on a miss `0x540d90` composites the tile (counter `+0x2d30`) into the shared 64x64 scratch surface `0x6d0ff0` (-> `0x541250`) -> blit `0x4b3280` -> unlock `0x4d27a0`. Pool created at `0x53f4f0` (`push 0x480000`, init `0x4d27c0`): 4.5 MB = 576 tiles of 64x64x16 bit, shared with other users (`0x53ffc4`, `0x54026d`, `0x5402d2`, `0x540640`, `0x540bc0`). 800x600 needs ~154 tiles, 1920x1080 ~527: the pool thrashes and ~150 tiles per frame get recomposited (verify run: ~5000 calls/s). ZaxHiRes sizes the pool to (W/64+2)*(H/64+2)*8 KB*3, min 4.5 MB (`/NoPoolFix` to compare); /LogFps also prints tiles composited per second (counted only while the compositor is hooked).

Result (2026-10-28, 1920x1080, overworld, vsync off, `/CompAvx2 /LogFps`): fps 84 80 80 79 79 80 79 77 73 79 (was 39-40 at the same spot with the 4.5 MB pool); tiles composited per second 610 in the first second (filling the cache), then 0-79 (only tiles scrolling into view; was ~5000/s).

### After the cache fix: where the frame time goes (2026-10-28, 1920x1080)
- perf at ~80-90 fps: Zax.exe used ~1.3 GHz of a core (thread ~60% busy). Hottest: the original row copy `0x437890` (48% of Zax cycles, memory-bound, ~2 cycles/byte), then logic and sprites.
- Off-CPU recording with sudo (`sched:sched_switch`, filtered on the thread, both directions) showed 57% in Sleep(), but the sudo password prompt stole focus and the message loop `0x5998f0` sleeps 1 or 100 ms per iteration while the window is inactive (`0x713e10` active, `0x713e11` minimised). Artefact. The game thread uses 32-bit int 0x80 syscalls (classic 32-bit Wine, not WoW64); dwarf unwinding of its user stacks doesn't work, frame pointers stop in libc.
- `/LogSleep` (IAT hook of Sleep at `0x5ea07c`, main thread only) in normal focused play: only `Sleep(0)` from `0x480bdc` once per frame, 0 ms; window active. fps a flat 88-90.
- No frame limiter in the game, DxWrapper (EnableVSync 0, no limit) or DXVK (no dxvk.conf): the ~90 fps cap comes from presentation (gamescope / display refresh / Steam frame limit). The game is no longer the bottleneck at 1080p.

## Sound listener (fixed 2026-10-28)
Listener update `0x4f2620` sets the 3D listener (`0x57c170`, stored at `0x7136a0..a8`) to (view dolly x + 320, -(y + 240), -10): `fadd [0x5eff98]` at `0x4f2712`, `fadd [0x5eff9c]` at `0x4f2763` (shared constants 320.0/240.0, also used by the aim cursor `0x470a6a`, zoom limits `0x4f24fd`, animation `0x526xxx`, so not patched). ZaxHiRes repoints those two operands to its own floats, set every frame by the frame hook (earlier in the same function) to play rect * zoom / 2, i.e. the centre of the world view.

## HUD bar (0x59e730, drawn every frame)
`/LogHud` (wraps the generic blit `0x4b3280`: thiscall dst, args src, x, y, flags; src rect via vtable `+0x90`) mapped the bar at 1920x1080: left panel 179x52 at 0 (`0x59ed06`), filler 160x58 at 179 (`0x59ed7f`, only drawn when width == 800, check at `0x59ed43`), right group 461x58 at W - 461 (`0x59ed31`), icons/text of the right group at 640-layout x + `[ebp-0x1c]` (ore/crystal icons `0x59efdf`/`0x59f13b`, text via `0x4a6470`/`0x4a66c0`, glyph blits `0x4a6693`), health icon `0x59f20a`, item `0x583ae1`/`0x583c13`, weapon `0x5acbe6`.
Design: 640 = 179 + 461. At 800 the filler (160) fills the gap; `[ebp-0x1c]` = filler width = W - 640 (right group contents), `[ebp-0x20]` = 0x50 = (W - 640) / 2, passed to the inventory renderers (`[ebp-0x24]+0x100` list, renderer vtable `+0x70`(index, dst, offset)) which place themselves at W/2 + constant; one renderer class gets -0x50 (`0x59fc2c`).
Fix in ZaxHiRes: call at `0x59ed7a` -> stub that sets `[ebp-0x1c]` = W - 640 and `[ebp-0x20]` = (W - 640) / 2, then blits the filler repeatedly across the W - 640 gap; `0x59fc2c` immediate -> -(W - 640) / 2.

## Inventory screen
CZaxInventoryScreen (vtable `0x5ee340`), created at `0x428c00` (SetSize 640x200; art is 800 wide with an 80-px rock margin each side; tabs/pages are child windows placed from data). Slides up from the bottom: `0x429564..0x4295be` moves it to y = parent bottom - slide offset (`+0x108`), x = -80 if width == 640 (hides the margins) else 0. The HUD bar slides up with it (bar y 1019 -> 894 at 1080p). Nothing draws beside it at higher resolutions, so that area keeps old pixels (black, or the bar's trail while sliding).
GUI windows are drawn on the canvas surface and copied to the frame (`0x5248c0`); the finished frame is copied to the display at `0x4f3124` (`call 0x4b3280`, dst `[esi+4]`). Surface Blt = vtable `0x5f4814 + 0x5c` = `0x4a8f40` (thiscall dst, src, src rect, dst rect, flags object; `0x6bb4e0` opaque, `0x6bcc68` with colour key).
Fix in ZaxHiRes: call at `0x4295be` -> `inv_move` (records the window, x 0 -> (W - width) / 2); call at `0x4f3124` -> `inv_present`, which while the window is on screen copies its 80-px left/right rock margins outwards to the screen edges in the frame (rows from the window top to the screen bottom), then presents.

## Game data and map format (2026-10-28)
`Game/DATA.dat` (718 MB) is a ZIP, all entries stored uncompressed, with one twist: the end-of-central-directory signature is `RE\x05\x06` instead of `PK\x05\x06`. Python's zipfile reads it through a wrapper that patches those two bytes on read (no file changes). 4982 files: `Cache/` (3365: `.mdl`/`.mdl16` models, `.frm` frames, `.seq` sequences), `Levels/` (824), `Sounds/`, `Resources/`, `Inventory/`, `Textures/`, `Music/`, `Movies/` (3 `.bik`), `Rules.txt`, `ZaxCredits.txt`.
Levels: `Levels/ACT 1..4/<mission>/` with per map `NN Name.zax` (level) + `NN Name.way` (binary, floats/ints; probably path data), a mission script `.RDS` (plain text: `:label` + `Speaker: line` dialogue) and `VOs/` voice lines (`.ogg`). Also `Levels/Multiplayer`, `Levels/Global`, `Inside Ship.ZAX`.
`.zax` is plain text, the engine's property serialisation (`ClassName` / `{` / `Key=Value` / nested objects, CRLF, tabs): root `CLayerSaveData` with Width/Height, Map Description, Environment Type, View Position, Team Info, Ambient Sounds, Path Data (`CWayPointMap`), Fog Of War, mini-map settings, Tree List, `Plasma Ground` (`CPlasmaTileMap`, likely the terrain layer map behind the tile compositor), Action Queue, and objects: 01 Main has 56760 lines, 1083 `CEntityBase`, AI (`CPatrolAreaAI`, `CChasePursueAI`, ...), triggers (`CTouchingPolygonTriggerAI`, oval), actions (`CZaxDialogAction`, `CDelayAction`, activate/deactivate), inventories.
Editor: not in the retail exe. Only leftovers: pause-menu labels `Load &Map (F9)`, `&Editor (F6)`, `&Tinker` (constructed, never used), per-object properties `Editor Only`, `Editor Display Offset`, `Render In Editor`, config `Show Overlays In Editor`. No editor classes. Engine: Reflexive's Velocity Engine; a Ricochet fan wiki lists Zax as shipped without a level editor.
