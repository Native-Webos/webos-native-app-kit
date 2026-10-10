# Remote control and input

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## Delivery path

```
LG remote (IR / BT Magic Remote) ─► kernel evdev ─► LSM (Wayland compositor)
    ─► wl_keyboard key event ─► SDL Wayland backend (LG fork or webosbrew fork)
    ─► SDL_KEYDOWN / SDL_KEYUP (+ SDL_TEXTINPUT, pointer, controller events)
```

- Keys reach the app as ordinary SDL key events with webOS-specific scancodes `[SRC]` (samples
  `sdl_shell_pump`; moonlight `navkey_sdl_webos.c`).
- SDL-webOS says its webOS scancode mapping *"can be found in decompiled keyboard_handle_key
  function as a series of int arrays"*, so it was reverse-engineered from LG's SDL
  (SDLW`include/SDL_scancode.h` ~L436) `[SRC]`.
- Some apps read `/dev/input/event*` directly for USB keyboards and mice, below the compositor
  (lgnome, punktfunk). This needs `EVIOCGRAB` care and is **not** needed for remote navigation `[SRC]`.

## Access policy: keys the system keeps unless asked

Hints must be set **before the window is created**. They are latched at window creation and
all-or-nothing for the session (punktfunk NOTES) `[SRC]`.

| Hint | Effect | System SDL (NDK `SDL_webOS.h`) | SDL-webOS 2.30.12 |
|---|---|---|---|
| `SDL_WEBOS_ACCESS_POLICY_KEYS_BACK` | Deliver Back to the app | yes | yes |
| `SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT` | Deliver Exit / long-Back | yes | yes |
| `SDL_WEBOS_ACCESS_POLICY_KEYS_HOME` | Deliver Home | **not declared** | yes |
| `SDL_WEBOS_ACCESS_POLICY_KEYS_GUIDE` | Deliver Guide | not declared | yes |
| `SDL_WEBOS_ACCESS_POLICY_KEYS_META` | Keyboard Super key (webOS 8+) | not declared | yes |
| `SDL_WEBOS_ACCESS_POLICY_RIBBON` | Launcher ribbon on Home | not declared | yes |
| `SDL_WEBOS_CURSOR_SLEEP_TIME` | Pointer auto-hide ms | yes | yes |

Sources: NDK sysroot `SDL2/SDL_webOS.h` `[BUILD]` (read here); SDLW`include/SDL_hints.h` L2720–2770 `[SRC]`.
PlxNative read the hint strings out of the system `libSDL2` on webOS 4.5 and found only BACK, EXIT
and FORCESTRETCH (plx `docs/remote-keys.md` §5) `[SRC]` (claim).

- Without KEYS_BACK, Back is consumed by the system and never reaches the app (all apps; plx
  `src/main.c` sets it with `setenv` before SDL init) `[SRC]`.
- Exit handling: punktfunk says *"Held Back is the EXIT key (375 in SDL3); a short tap is ordinary
  Back… Needs both KEYS_EXIT and KEYS_BACK, or gesture SIGTERMs app"* `[SRC]` (claim). lgnome leaves
  Exit to the system and shuts down on `SDL_QUIT` `[SRC]`.
- Capturing HOME is opt-in in moonlight-tv (`syskey_capture` setting) `[SRC]`. PlxNative argues an
  app should not take the TV's own navigation keys `[SRC]`. **Kit default: do not request HOME.**

## Scancodes

### SDL-webOS 2.x (webosbrew), SDLW`include/SDL_scancode.h` `[SRC]`

| Key | Scancode |
|---|---|
| Home | 384 |
| CH+ / CH− | 480 / 481 |
| **Back** | **482** |
| Cursor show / hide (Magic Remote pointer, sent as keys) | 484 / 485 |
| Red / Green / Yellow / Blue | 486 / 487 / 488 / 489 |
| Guide | 495 |
| **Exit** | **505** |
| webOS 1–12 | 340–351 |
| D-pad, OK | standard `SDL_SCANCODE_UP/DOWN/LEFT/RIGHT/RETURN` `[SRC]` (plx measurements, system SDL) |

The Linux evdev table in SDL-webOS maps `KEY_PLAY`→`AUDIOPLAY`, `KEY_PLAYPAUSE`→`AUDIOPLAY`,
`KEY_PAUSE`→`PAUSE`, `KEY_STOP`→`STOP`, `KEY_FASTFORWARD`→`AUDIOFASTFORWARD`,
`KEY_REWIND`→`AUDIOREWIND` (`src/events/scancodes_linux.h`) `[SRC]`. **Whether LG's compositor delivers
transport keys through that table or through the webOS block is `[UNK]`.** It needs a key capture
on the TV with the SDL we ship.

### LG system SDL 2.0.4 on webOS 4.5 (plx measurements) `[SRC]` (claim), `[UPSTREAM-HW]`

Back 482, Exit 505, Play 450, FF 451, RW 452, Pause 72 (`SDL_SCANCODE_PAUSE`), PlayPause 261,
Stop 120/260, CH 300/301, pointer hidden 484. The colour-key codes from the offline table were wrong
when measured on the device. **These codes are not portable to another SDL build.**

### SDL3 webOS fork (punktfunk) `[SRC]`

The webOS block is compacted to 352–375: Home 364, Back 367, colours 370–373, Guide 374, Exit 375.
Colour keys and Back are matched on the raw evdev code, because rust-sdl3's enums cannot name them.

## Event semantics

- **Repeat:** `SDL_KeyboardEvent.repeat` is set on auto-repeat `[SRC]` (SDL). PlxNative reads repeat
  from bit `0x100` of the state word in the fork's shifted layout, because it uses stock headers
  against the broken-ABI library `[SRC]`. With matching headers, `repeat` is a normal field `[INF]`.
- **Long press:** the TV turns a held Back into Exit on its own; punktfunk advises against timing
  it yourself `[SRC]` (claim). Other long-press behaviours are `[UNK]`.
- **Unbound keys must have no side effects** (plx `remote-keys.md` §6, LG self-checklist row 40) `[SRC]`.
- **Pointer (Magic Remote):** SDL mouse events; `SDL_webOSCursorVisibility` hides the arrow; the
  `CURSOR_SHOW`/`HIDE` key events announce pointer mode changes `[SRC]`. Pointer coordinates are in
  window units (punktfunk) `[SRC]`.
- **Text input:** an LG IME path exists. SDL-webOS has `SDL_waylandwebos_osk.c` `[SRC]`. PlxNative found
  `SDL_TextInputEvent` shifted by the same `inputSource` field (`rust-modules/src/textinput.rs`) `[SRC]`.
- **Gamepads:** SDL GameController via the bundled SDL; moonlight ships a `gamecontrollerdb.txt` `[SRC]`.
  On webOS 23+ pads may type as remote keys (punktfunk) `[SRC]` (claim).

## Focus and navigation: what the references do

| Project | Approach |
|---|---|
| moonlight-tv | LVGL groups + a key-to-`NAVKEY` mapping layer (`navkey_sdl.c`, `navkey_sdl_webos.c`) `[SRC]` |
| PlxNative | `classify(sym, wcode) → Key`, then route handlers, then a global ladder (`ui/consts.rs`, `app/run.rs`) `[SRC]` |
| punktfunk | Shared console shell handles focus `[SRC]` |

The shared pattern: **raw event → platform key adapter → semantic key → focus/navigation.** Widgets
never see raw codes. moonlight's `navkey` layer is the closest C example. The mission's input
pipeline matches that evidence.

## Kit implications

1. A platform `KeyMapper` per SDL build (system-SDL table vs SDL-webOS table) produces a raw
   `KeyEvent{scancode, keycode, down, repeat, timestamp}` and a semantic `Action`.
2. A mapping table with provenance per entry (source / measured-on-device / unknown), like plx's map.
3. A key-capture diagnostic screen in Example A, to record real codes on the target TV and store them
   in the compatibility DB.
4. Repeat handling, unknown-key swallowing and Back handling are tested on the host with synthetic events.
