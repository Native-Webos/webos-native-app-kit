# webOS platform (retail TV, native apps)

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## Process model

- A native app is an ELF executable named by `main` in `appinfo.json` with `"type": "native"`.
  The application manager (SAM) launches it inside a jail `[SRC]` (plx `paths.rs`, punktfunk NOTES).
- Launch parameters arrive as **`argv[1]`, a JSON string**. No environment variables can be passed
  through a SAM launch (punktfunk NOTES "Runtime gotchas") `[SRC]` (claim). moonlight-tv parses launch
  parameters in `app_handle_launch(&app, argc, argv)` `[SRC]`.
- `APPID` is set in the environment by the launcher. webosbrew SDL reads `SDL_getenv("APPID")` to
  register with SAM and fails with "APPID environment variable is not set" otherwise
  (SDLW`src/core/webos/SDL_webos_init.c`) `[SRC]`.
- `EGL_PLATFORM` and `XDG_RUNTIME_DIR` are **not** set for native apps. Every app studied sets
  `EGL_PLATFORM=wayland` and `XDG_RUNTIME_DIR=/tmp/xdg` itself before SDL video init `[SRC]`.

## Install locations and jails `[SRC]` (plx `rust-modules/base/src/paths.rs`, claims)

| Installed by | App dir | Jail profile |
|---|---|---|
| Developer Mode (`ares-install`) | `/media/developer/apps/usr/palm/applications/<id>` | `jail_native_devmode.conf` (rw `/media/developer`, ro `/media/internal`) |
| Homebrew Channel | `/media/cryptofs/apps/usr/palm/applications/<id>` | `jail_native.conf` (rw `/media/internal`, no `/media/developer`) |

- PlxNative says LG's jail config sets `HOME` twice, and which value survives differs per profile.
  It resolves the app dir from `/proc/self/exe` instead. Kodi's `CPlatformWebOS::GetHomePath()` is
  said to do the same `[SRC]` (claim; Kodi source not read).
- The app directory is replaced wholesale on every (re)install, so files written there are lost on
  update (plx `session_candidates` doc) `[SRC]` (claim).
- `/tmp` is mode 1777, writable in both profiles, shared with other apps and cleared on reboot `[SRC]` (claim).
- The jailed uid is in group 5000. Files created 0644 in `/tmp` are readable by co-resident apps.
  PlxNative creates logs 0640 with `O_NOFOLLOW` plus `fstat` ownership and link-count checks (PLX`src/main.c` `open_fd_log`) `[SRC]`.

**Our position:** paths come from a platform adapter that probes candidates at runtime and reports
which are durable. Nothing is hard-coded in app code. See NETWORKING-AND-STORAGE.md.

## Device information sources `[SRC]` (punktfunk `src/platform/webos/device.rs`)

| Source | Content | Notes |
|---|---|---|
| `/var/run/nyx/os_info.json` | webOS version, SDK version | Optional; jail readability varies |
| `/var/run/nyx/device_info.json` | model, etc. | Optional |
| `/etc/prefs/properties/machineName` | SoC/board codename (`k5lp`, `m16p`) | Optional |
| `luna://com.webos.service.tv.systemproperty/getSystemInfo` | `otaId`, model | Luna call (punktfunk mentions `otaId` from `getSystemInfo`) |
| `SDL_webOSGetPanelResolution`, `SDL_webOSGetRefreshRate` | Panel size, refresh | Present in system SDL from 4.4.2 `[FWDB]` and in SDL-webOS `[SRC]` |

punktfunk's policy, which we adopt: detect by attempt where possible; use version facts only for
choices that cannot be probed; never branch on model strings `[SRC]`.

## Luna service bus

- Libraries present on every firmware in the inventory: `libluna-service2.so.3`, `libhelpers.so.2`
  (`HLunaServiceCall`), `libpbnjson_c.so.2` `[FWDB]`.
- webosbrew SDL calls `luna://com.webos.applicationManager/registerApp` (lifecycle v2) or
  `…/registerNativeApp` (v1) through `HELPERS_HLunaServiceCall` (SDLW`src/core/webos/SDL_webos_init.c`
  `registerApp`) `[SRC]`. It also registers a screensaver request and a power-state subscription `[SRC]`.
- moonlight-tv launches Home with
  `HLunaServiceCallSync("luna://com.webos.applicationManager/launch", {"id":"com.webos.app.home","params":{"launchType":"homeKey"}})`
  (MLT`src/app/platform/webos/app_webos.c` ~L86) `[SRC]`.
- punktfunk drives Luna through both the library (`ls2.rs`) and a bounded `luna-send` subprocess (`luna.rs`) `[SRC]`.
- The webOS **OSE** service name `com.webos.service.applicationmanager` differs from retail
  `com.webos.applicationManager` `[SRC]` (OSE sample vs SDL-webOS). Never copy OSE URIs to retail.

## Graphics stack

- Wayland compositor (LSM) with LG's `wl_webos_shell` extension. `libwayland-webos-client.so.1` is on
  all inventoried firmware `[FWDB]`. SDL's Wayland backend sets shell-surface properties such as
  `_WEBOS_ACCESS_POLICY_*` and `_WEBOS_WINDOW_CLASS` (SDLW`src/video/wayland/SDL_waylandwebos.c`) `[SRC]`.
- EGL + GLES2 on every firmware `[FWDB]`. GPU vendor/extension set `[UNK]`; plx references Mali
  hardware counters (`tools/mali-hwcnt-probe.c`), suggesting Mali on at least the author's set `[INF]`.
- Video is composited on a separate hardware plane under the UI. The UI clears transparent where
  video shows ("punch-through") `[SRC]` (samples `sdl_shell_present_transparent`; punktfunk NOTES).

## Platform behaviours to respect `[SRC]` (claims from upstream notes)

- Renicing or raising thread priority needs `CAP_SYS_NICE`/`RLIMIT_NICE`, which the SAM jail does not grant (punktfunk).
- `getrandom(2)` via libc `dlsym` is missing on old glibc. `/dev/urandom` is not exposed in the jail (punktfunk `.cargo/config.toml` comment).
  **Impact:** our RNG source must use the raw `getrandom` syscall where the kernel supports it.
  Kernel version per firmware is `[UNK]`.
- Showing or hiding the window while NDL composites can kill the process (punktfunk).
- Developer Mode sessions expire; apps installed through it are removed on expiry (plx `distribution.md`).
