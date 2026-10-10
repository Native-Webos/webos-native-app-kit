# Native rendering

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## What the reference projects do

| Project | Window | GL / renderer | UI drawing | Text |
|---|---|---|---|---|
| PlxNative | System SDL2 | GLES2, own renderer (Rust) | Own retained UI, 60 fps claimed on 2019 set | Bundled TTF fonts in `pkg/` (`appfont*.ttf`) |
| moonlight-tv | Bundled SDL 2.30 | `SDL_Renderer` (`SDL_HINT_RENDER_SCALE_QUALITY=1`, MLT`src/app/lvgl/lv_disp_drv_app.c`) | LVGL 8 fork | FreeType via LVGL |
| punktfunk | Bundled SDL3 | Own GL context, Skia | Shared punktfunk shell | Skia |
| lgnome | System SDL2 | SDL | LVGL | LVGL fonts (IBM Plex, JetBrains Mono, OFL) |
| samples | System SDL2 | `SDL_CreateRenderer(…ACCELERATED)` | Transparent clear only | — |

All `[SRC]`.

## Window creation `[SRC]`

- `SDL_WINDOW_FULLSCREEN`, not `FULLSCREEN_DESKTOP`. Per samples `sdl_shell.c`, *"on webOS the
  desktop-fullscreen path does not give the app the whole panel"*. moonlight sets
  `FEATURE_WINDOW_FULLSCREEN_DESKTOP OFF` and `FEATURE_FORCE_FULLSCREEN ON` for webOS.
- Size from `SDL_GetCurrentDisplayMode(0)`. Stock SDL reports the **app plane** (1080p) on 4K sets.
  `SDL_webOSGetPanelResolution` gives the panel size, which matters for video placement (punktfunk
  NOTES "Known platform limitations"; samples `sdl_shell_panel_size`).
- Optional webOS SDL symbols are declared weak (samples) or dlsym'd (plx) so the binary still loads
  on firmware without them.
- SDL-webOS's Wayland shell surface always sets `_WEBOS_ACCESS_POLICY_FORCESTRETCH=true`
  (SDLW`src/video/wayland/SDL_waylandwebos.c` ~L179).

## Coordinate system and scaling

- UI plane 1920×1080 is the common case `[INF]` (SDL reports 1080p on 4K panels per punktfunk).
  Drawable size can differ from the display mode; pointer events arrive in window units and must be
  scaled to surface pixels (punktfunk NOTES "UI rendering") `[SRC]`.
- **Decision input:** use a fixed logical canvas (1920×1080 units) mapped to the drawable size at
  runtime. Video placement uses panel coordinates from `SDL_webOSGetPanelResolution`, kept apart
  from UI coordinates (punktfunk: *"Decouple decode dimensions from punch-through rect"*).

## Presentation and pacing `[SRC]` (punktfunk NOTES)

- Swap interval 1 (vsync) in menus. Immediate while streaming, because the same thread forwards
  input. A UI app can use vsync throughout.
- `SDL_GetCurrentDisplayMode` may report `refresh_rate=0`; clamp to a default.
- The panel refresh rate cannot be set from an app.

## Video plane composition `[SRC]`

- The UI must clear to **RGBA(0,0,0,0)** where video should show. The window must stay mapped;
  a hidden window receives no pointer input (punktfunk).
- Over video there is no framebuffer to read back, so blur/backdrop effects cannot sample video (punktfunk).
- Exported-window placement on webOS 5+: `SDL_webOSCreateExportedWindow(VIDEO)` →
  `SDL_webOSSetExportedWindow(id, src, dst)` (plx `vp_place`; samples `media/smp/webos5`).

## GPU capabilities `[UNK]`

No GL extension list, max texture size or shader precision data from a real set was found in the
sources read. M3 must log `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, `GL_EXTENSIONS`,
`GL_MAX_TEXTURE_SIZE` and the precision formats at startup. That gives the evidence; we should not
guess. PlxNative's tooling probes Mali hardware counters, which suggests Mali GPUs on some sets `[INF]`.

## Text

- Bundling TTF/OTF fonts is the universal practice (plx, lgnome; moonlight uses an icon font) `[SRC]`.
- FreeType `libfreetype.so.6` exists on every firmware `[FWDB]`, but PlxNative notes silent fallbacks
  when bundled font paths failed under one jail profile (paths.rs doc) `[SRC]`.
- **Decision input:** rasterise glyphs ourselves from a bundled font into a GPU atlas. The candidate
  is `stb_truetype` (public domain/MIT, single header, no runtime dependency, same output on host and
  TV). FreeType stays an option if hinting quality proves insufficient.

## Images

- moonlight uses `SDL2_image` (bundled with its SDL). punktfunk uses the Rust `image` crate and
  normalises covers to ≤480×720 JPEG on the fetch thread, never on the render thread `[SRC]`.
- `libjpeg` SONAME changes between firmware generations (`.so.8` → `.so.62`) `[FWDB]`. A
  self-contained decoder avoids that dependency.
- **Decision input:** `stb_image` (PNG/JPEG) on worker threads to CPU RGBA. Upload to GL only on the
  render thread. Max decoded size bounded by the request.

## Resource recovery

- GL context loss on webOS (for example across suspend) is `[UNK]`. Design textures to be re-creatable
  from CPU-side sources or cache, and log any `SDL_RENDER_DEVICE_RESET`/context errors.
