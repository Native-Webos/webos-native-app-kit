# Debugging and testing

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## Logging on the device `[SRC]`

| Project | Approach |
|---|---|
| PlxNative | Event log, crash log and stderr capture under the runtime dir (`/tmp` or `/tmp/<id>`), opened 0640 with `O_NOFOLLOW` + ownership/link checks; event lines scrubbed of hosts, tokens and GUIDs before write |
| lgnome | `freopen(LGNOME_NATIVE_LOG_PATH or /tmp/lgnome-native.log, "w", stderr)`, line-buffered; categorised `clog`; `tools/check-logging-policy.sh` |
| moonlight-tv | commons-c logging, routes SDL logs (`SDL_LogSetOutputFunction`) and PmLog (`PMLOG_LIBRARIES`) |
| punktfunk | `tracing` + file appender; telemetry level by env |

- Reading logs needs Developer Mode access. Without root, webOS Dev Manager reads files as a user in
  the app's group, hence 0640 (plx `main.c` comment) `[SRC]` (claim). `ares-log` exists in the CLI
  (not verified here) `[UNK]`.
- `luna-send` over plain ssh needs `ssh -tt` (punktfunk) `[SRC]` (claim).

## Crash diagnostics `[SRC]`

- PlxNative: async-signal-safe tracer writing PC, signal and one `/proc/self/maps` line; offline
  stackwalk tool `tools/plxnative-stackwalk.c`; Sentry Native optional.
- lgnome: emergency media release, then re-raise.
- moonlight-tv: `RLIMIT_CORE=0` in release builds.
- **Kit:** minimal async-signal-safe crash record (signal, fault address, build id, backtrace via
  `backtrace()` only if proven safe; otherwise raw frame-pointer walk later). Release media first.

## Host simulation `[SRC]`

- PlxNative: Linux/macOS simulator of the full UI with mock servers; screenshot regression; explicit
  note that it **cannot** answer frame rate, LG text rasterisation or video-plane behaviour.
- punktfunk: Docker container with a virtual 1080p display over VNC; llvmpipe timing differs from the TV.
- webosbrew/samples: `media/common` builds on the host; stream parsers checked against `ffprobe`.
- moonlight-tv: tests with Unity on non-webOS builds (`BUILD_TESTS` defaults ON off-device).

## Device workflow (to verify with a TV) `[SRC]` (claims)

1. Developer Mode app on the TV, then `ares-setup-device`, then `ares-install <ipk>`, then `ares-launch <id>`.
2. Launch through SAM (`ares-launch` or `luna-send …/applicationManager/launch`), **not** by running
   the binary over ssh, or video punch-through will not composite (punktfunk).
3. Pull logs with ssh/scp (rooted) or `ares-log` / Dev Manager (unrooted).
4. Symbol-verify the ipk before installing (`webosbrew-ipk-verify` or our equivalent).

## Test methodology for the kit

| Level | What | Where |
|---|---|---|
| Unit | State machines, input mapping, focus navigation, layout, navigation stack, JSON, config, storage (in temp dirs), cache eviction, cancellation, HTTP error mapping, media controller with a scripted backend | Host, CTest, every build |
| Sanitizers | Same suite under ASan/UBSan | Host CI preset |
| Integration (host) | SDL window with offscreen/dummy video driver, renderer smoke, real HTTP against a local test server | Host |
| Golden images | Renderer output via `glReadPixels` on Mesa (llvmpipe) | Host; not proof of TV rendering |
| Cross build | Every webOS preset compiles and links; import verification against firmware inventory | Host (NDK) |
| Device | Install, launch, key capture, focus, network, paths probe, NDL init, playback | TV, recorded in the compatibility DB |

Nothing is marked passed on the device unless it was run on a TV and the record names the model,
firmware and date.
