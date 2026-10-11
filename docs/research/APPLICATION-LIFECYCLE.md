# Application lifecycle

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## Startup sequence observed across apps `[SRC]`

```
main(argc, argv)                          argv[1] = launch params JSON (punktfunk, moonlight)
├─ open logs / redirect stderr            (plx, lgnome)
├─ install fatal-signal handlers          (plx crash tracer; lgnome NDL emergency release)
├─ signal(SIGPIPE, SIG_IGN)               (lgnome)
├─ setenv EGL_PLATFORM=wayland, XDG_RUNTIME_DIR=/tmp/xdg   (moonlight, lgnome, samples)
├─ [release] setrlimit(RLIMIT_CORE, 0)    (moonlight)
├─ SDL_Init(0)                            → SDL-webOS: loads webOS libs, registers app with SAM
├─ set webOS hints (KEYS_BACK, KEYS_EXIT, cursor sleep, …)
├─ load settings
├─ (media backend early init, see NATIVE-MEDIA.md ordering notes)
├─ SDL_InitSubSystem(SDL_INIT_VIDEO)      → window, GL context
├─ input init (game controllers)
└─ UI open → main loop
```

## SAM registration and lifecycle events (SDL-webOS 2.30.12) `[SRC]`

Traced in the bundled-SDL path (moonlight-tv uses this SDL):

1. `SDL_Init` → `SDL_webOSLoadLibraries()` → `SDL_webOSRegisterApp()` (SDLW`src/SDL.c` ~L231–243).
2. `SDL_webOSRegisterApp` (SDLW`src/core/webos/SDL_webos_init.c`): unless `SDL_WEBOS_REGISTER_APP`
   is false, reads `APPID`, reads `nativeLifeCycleInterfaceVersion` for the app, then calls
   `luna://com.webos.applicationManager/registerApp` (v2) or `registerNativeApp` (v1). Also
   registers a screensaver request (`<appId>.wakelock`) and a power-state subscription.
3. Lifecycle v2 callback: `"relaunch"` → `SDL_RaiseWindow`; `"close"` → `SDL_SendQuit()` → `SDL_QUIT`.
4. Wayland shell state (SDLW`src/video/wayland/SDL_waylandwebos.c` ~L183–226):
   - about-to-minimise → `SDL_APP_WILLENTERBACKGROUND`
   - minimised → `SDL_WINDOWEVENT_MINIMIZED`, `SDL_APP_WILLENTERBACKGROUND`, `SDL_APP_DIDENTERBACKGROUND`
   - fullscreen again → `SDL_WINDOWEVENT_RESTORED`, `SDL_APP_WILLENTERFOREGROUND`, `SDL_APP_DIDENTERFOREGROUND`
   - shell close → flush all events, `SDL_WINDOWEVENT_CLOSE`
5. `SDL_Quit` → `SDL_webOSUnregisterApp()`.

How LG's **system** SDL handles registration is `[UNK]`: no source exists. PlxNative and lgnome run
on it, so it must allow a normal launch `[INF]`.

`nativeLifeCycleInterfaceVersion` in `appinfo.json`: plx `distribution.md` calls it *"a webOS OSE
property — no shipped native TV app sets it"* `[SRC]` (claim). SDL-webOS nevertheless reads it to pick
the registration URI `[SRC]`. How retail SAM behaves for each value is `[UNK]`.

## Background / foreground

- moonlight-tv interrupts streaming on `SDL_APP_WILLENTERBACKGROUND`, window focus loss (when
  fullscreen) and `SDL_WINDOWEVENT_HIDDEN`; it invalidates the screen on `DIDENTERFOREGROUND` and
  `EXPOSED` `[SRC]`.
- PlxNative's checklist distinguishes cold relaunch from app switch with the process still alive
  (suspended playback resume) `[SRC]`. So the process can stay alive in the background, at least on
  the author's set `[UPSTREAM-HW]` (claim).
- NDL punch-through only composites for the SAM **foreground** app (punktfunk) `[SRC]`.
- **Kit position:** expose `Suspended`/`Resuming` states driven by the SDL app events above. Media must
  pause or release on background. Do not claim true suspend/resume support (process frozen and
  thawed) until measured on a device.
- **Measured on an OLED65C1PTB (webOS 6.5.3), 2026-10-11** `[HW]`: launching another app over a kit app
  delivers `SDL_WINDOWEVENT_MINIMIZED`, `SDL_APP_WILLENTERBACKGROUND`, `SDL_APP_DIDENTERBACKGROUND` and
  focus loss in the same millisecond; relaunching it delivers `SDL_WINDOWEVENT_RESTORED` and
  WILL/DIDENTERFOREGROUND. The process keeps running in between (not frozen): the app shell stops
  drawing itself. A first WILL/DIDENTERFOREGROUND pair also arrives just after the first frame of a cold
  launch. `ares-launch --close` arrives as `SDL_QUIT`.

## Termination

- Normal: Exit key / `"close"` lifecycle event / Wayland close → `SDL_QUIT` → app loop exits → teardown.
- SIGTERM/SIGINT: SDL installs graceful handlers that raise `SDL_QUIT` **only if the disposition is
  `SIG_DFL`** (lgnome comment) `[SRC]`. If we install our own SIGTERM handler, it must only set an atomic
  flag and wake the loop.
- Fatal signals: lgnome releases NDL from the handler (on `sigaltstack`, with `alarm(3)`), then
  re-raises so crash reporting sees the real signal. PlxNative writes a crash record with
  async-signal-safe code only `[SRC]`.
- punktfunk: if KEYS_EXIT is not requested together with KEYS_BACK, the Exit gesture SIGTERMs the
  app `[SRC]` (claim).
- How long SAM waits between SIGTERM and SIGKILL is `[UNK]`. Teardown must be bounded: cancel
  workers, give them a fixed join deadline, release media first.

## moonlight-tv teardown order (MLT`src/app/app.c` `app_deinit`) `[SRC]`

`app_bus_drain` → session destroy → UI close/deinit → wakelock off → input deinit →
`SDL_QuitSubSystem(VIDEO)` → backend destroy → settings save → media module quit → `SDL_Quit` → logging deinit.

Draining the bus first means no queued main-thread callback runs against destroyed objects. Media
goes before video, and SDL last.

## Kit lifecycle model

```
Created → Initialising → Ready → Running ⇄ Suspended → Resuming → Running
                                   └──────────────► Stopping → Terminated
```

- Transitions are driven only from the main thread.
- `Stopping`: (1) stop accepting new async work; (2) cancel tokens; (3) release media; (4) drain the
  main-thread queue once without running UI callbacks for destroyed owners; (5) join workers with a
  deadline; (6) destroy GPU resources; (7) destroy the window; (8) flush logs.
- Workers never block on the UI thread (no `post_sync` from workers during shutdown). moonlight's
  `app_bus_post_sync` is the pattern to avoid during `Stopping` `[SRC]`.
