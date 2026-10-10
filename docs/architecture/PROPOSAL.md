# Architecture proposal

Derived from [`../research/`](../research/README.md); decisions are recorded in
[`../research/ARCHITECTURE-DECISIONS.md`](../research/ARCHITECTURE-DECISIONS.md). Nothing here is
implemented yet.

## Layers and dependency direction

```
applications/            examples A–E, later real apps            (depend on everything below)
   │
services/  (optional)    auth, session                            (no UI, no platform code)
   │
kit/                     ui · focus · navigation · image · net · media(controller)
   │                     async · storage · config · logging · diagnostics · capabilities
   │
capabilities/            Renderer · InputSource · AudioOut · MediaBackend · HttpTransport
   │                     PathProvider · DeviceInfo · Clock               (interfaces only)
   │
platform/                host/  (SDL2 system, Mesa GLES2, libcurl, POSIX paths)
                         webos/ (SDL-webOS bundled, GLES2, dlopen NDL/curl, jail-aware paths, Luna)
```

Rules (checked by CMake target dependencies; a target cannot include headers it does not link):

- `kit/*` never includes SDL, GL, curl or webOS headers. Only `platform/*` and `render/gles2` do.
- Widgets never see raw key codes, never call GL, never do I/O.
- Media controller and `MediaSource` know nothing about Jellyfin/Plex/etc.
- Each optional subsystem is a separate static library target.

## Planned module layout (created only as each milestone implements it)

```
webos-native-app-kit/
├── CMakeLists.txt, CMakePresets.json, cmake/
├── platform/{core,host,webos}/        entry, env, signals, paths, device info, Luna (webos)
├── render/{core,gles2}/               renderer interface + GLES2 backend, font atlas
├── input/                             KeyEvent, Action, mapping tables, repeat handling
├── ui/                                View tree, layout, widgets, focus manager, navigation stack
├── async/                             WorkerPool, MainDispatcher, CancellationToken, Future-ish
├── net/                               HttpClient interface, curl backend, errors, retry policy
├── storage/                           PathProvider use, atomic file, KV prefs, disk cache, SecretStore
├── image/                             ImageService (fetch → decode → cache → texture request)
├── media/{core,backends/ndl,…}/       MediaSource, PlaybackController, backend interface
├── logging/, diagnostics/, capabilities/
├── applications/examples/{hello,tvui,net,player}/
├── tests/{unit,integration,device}/
├── tools/                             doctor, build-host, build-webos, package, verify, install, launch, logs, test, clean
├── third_party/                       SDL-webOS (fetched at pinned tag), stb, doctest, json
└── docs/
```

## Runtime structure

```
main thread                                    worker pool (N threads)        net thread
───────────                                    ───────────────────────        ──────────
App::run()                                     blocking file I/O, decode      curl multi loop
 ├─ poll SDL events → InputAdapter → Action    │                              │
 ├─ FocusManager / NavigationStack / UI        │  post(result) ──────────┐    │ post(result) ─┐
 ├─ MainDispatcher.drain() ◄────────────────────────────────────────────┴──────────────────────┘
 ├─ animations(tick)
 ├─ layout → render (GLES2) → SDL_GL_SwapWindow
 └─ lifecycle transitions
```

- Callbacks hold a `LifetimeToken` (`weak_ptr`) of their owner and are skipped if it expired.
- `Stopping` cancels all tokens, then joins workers with a deadline, so no worker waits on the main thread.

## Key interfaces (sketch, to be refined when implemented)

```cpp
// input
enum class Action { Up, Down, Left, Right, Confirm, Back, Exit, PlayPause, Play, Pause, Stop,
                    Rewind, FastForward, ChannelUp, ChannelDown, Red, Green, Yellow, Blue, Unknown };
struct KeyEvent { int scancode; int keycode; bool down; bool repeat; uint64_t time_ms; };

// render
class Renderer {
 public:
  virtual void begin_frame(Size drawable) = 0;
  virtual void clear(Color) = 0;                 // Color{0,0,0,0} punches through to the video plane
  virtual void push_clip(Rect) = 0;  virtual void pop_clip() = 0;
  virtual void push_transform(Affine) = 0;  virtual void pop_transform() = 0;
  virtual void fill_rect(Rect, Color, float radius = 0) = 0;
  virtual void draw_image(TextureId, Rect src, Rect dst, float alpha) = 0;
  virtual void draw_text(const TextRun&, Point, Color) = 0;
  virtual TextureId create_texture(const ImageRGBA&) = 0;   // main thread only
  virtual void destroy_texture(TextureId) = 0;
  virtual void end_frame() = 0;
};

// async
class CancellationToken { public: bool cancelled() const; void on_cancel(std::function<void()>); };
class MainDispatcher { public: void post(std::function<void()>); void drain(size_t budget); };

// net
struct HttpRequest { std::string method, url; Headers headers; std::string body;
                     Duration timeout; size_t max_response_bytes; bool idempotent; };
class HttpClient { public: virtual RequestHandle send(HttpRequest, CancellationToken,
                                                     std::function<void(HttpResult)> on_main) = 0; };

// storage
enum class Durability { Durable, Volatile };
struct StorageLocation { std::filesystem::path path; Durability durability; };
class PathProvider { public: virtual StorageLocation config() = 0; /* data, cache, temp, logs */ };

// capabilities
enum class Support { Supported, Unsupported, Unknown };
```

## Host vs TV

| Concern | Host | webOS |
|---|---|---|
| SDL | system 2.30.0 | bundled SDL-webOS 2.30.12 (standard ABI) |
| GL | Mesa EGL + GLES2 | firmware EGL + GLES2 |
| Keys | keyboard map (arrows/Enter/Backspace/Esc/F-keys emulate remote) | SDL-webOS scancode table |
| Paths | XDG dirs or `--data-dir` | probed jail candidates |
| HTTP | `libcurl.so.4` via dlopen (system) | `libcurl.so.4` via dlopen (firmware) |
| Media | scripted test backend; optional real software backend later | NDL v2 (dlopen) |
| Device info | `/etc/os-release`, GL strings | nyx JSON files, SDL webOS calls, GL strings |
