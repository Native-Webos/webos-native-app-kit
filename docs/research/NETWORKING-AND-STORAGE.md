# Networking and storage

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## HTTP / TLS stacks used

| Project | HTTP | TLS | CA roots |
|---|---|---|---|
| PlxNative | libcurl (resolved at runtime, per `fwcompat.py` docstring) | via curl | TV store **plus** bundled `pkg/le-roots.pem`, consulted only for `*.plex.direct` after the TV store reports an unknown issuer (`docs/release-audits/v0.8.0.md`) |
| moonlight-tv | libcurl (system) + a placeholder SONAME library "for cURL ABI issue" | mbedTLS built from source for its own crypto | — |
| punktfunk | Rust `ureq` + `rustls` | rustls (aws-lc-rs) | `webpki-roots` bundled |
| lgnome | IronRDP (Rust) | Rust TLS | — |

All `[SRC]`.

### Firmware facts `[FWDB]`

- `libcurl.so.4` from webOS 5.3.1 (curl ABI 4.5.0 → 4.8.0); absent from the 4.x and older images
  (they have other curl SONAMEs).
- webOS 6.4.0 curl `DT_NEEDED`: `libcares.so.2`, `libcrypto.so.1.1`, `libssl.so.1.1`,
  `libnghttp2.so.14`, `libz.so.1`. The TV's curl brings its own TLS stack.
- OpenSSL moves 1.1 → 3 at webOS 9. **Never link OpenSSL directly.** A dynamically linked
  `libcurl.so.4` hides the change.
- NDK sysroot curl 4.8.0 `NEEDED`: `libssl.so.1.1`, `libcrypto.so.1.1`, `libz`, `libatomic`, … `[BUILD]`.
  Only the SONAME `libcurl.so.4` ends up in our binary; the TV's copy is used at runtime `[INF]`.
- The NDK headers are curl 8.x-era (4.8.0 ABI) and the TV has 4.5.0 on webOS 5/6. Using a curl
  option or function newer than the TV's curl fails at load time (function) or at runtime (option →
  `CURLE_UNKNOWN_OPTION`) `[INF]`. Use only long-standing `curl_easy_*` / `curl_multi_*` APIs and check
  return codes.
- moonlight's placeholder `libcurl` (commons-c `platform/webos/curl-abi-fix`) is a tiny SONAME-named
  library with a no-op constructor, beside a "sys-placeholder" whose constructor calls `abort()`.
  **The problem it solves is undocumented** `[UNK]`. We will not copy this without understanding it.

### CA trust `[SRC]` (claim) / `[UNK]`

PlxNative found that older firmware trust stores lack roots that current servers chain to (Let's
Encrypt for `*.plex.direct`). The system CA path on webOS 6 and its freshness are `[UNK]`. **Kit
design:** TLS verification is always on; the HTTP client accepts an optional app-supplied CA bundle
added to the system store; the system CA path is probed and logged.

### Other network notes `[SRC]` (claims)

- `SIGPIPE` must be ignored (lgnome), or a dropped peer kills the process.
- mDNS discovery works on the LAN from an app (punktfunk `mdns-sd`; moonlight `libmicrodns`).
- Wake-on-LAN works via raw UDP (punktfunk).

## Storage

### Writable locations per jail (plx `paths.rs`) `[SRC]` (claim, see WEBOS-PLATFORM.md)

| Location | Dev Mode jail | Homebrew jail | Survives reinstall | Survives reboot |
|---|---|---|---|---|
| App dir (`/proc/self/exe` dirname) | writable? (moonlight writes `$HOME/conf`) | `root:5000 0755`, **not** writable | **No**, replaced on (re)install | yes |
| `/media/developer/…` | writable | absent | yes | yes |
| `/media/internal/…` | read-only | writable | yes | yes |
| `/tmp/…` | writable (1777, shared) | writable (1777, shared) | n/a | **no** |

Conflicts between sources:

- moonlight-tv uses `$HOME/conf` and `$HOME/cache`, falling back to `/tmp/moonlight-tv/…` and
  flagging the result non-persistent (`path_webos.c`) `[SRC]`.
- PlxNative says `$HOME` is set twice by the jail config and is unreliable; app-dir writes vanish on
  update. It reports a webOS 4.4.3 Dev Mode set where neither `/media/developer`, `/media/internal`
  nor the app dir accepted writes `[SRC]` (claim).

**Writable, durable storage on webOS 6 is `[UNK]` until probed on the device.**

### Kit design (from the evidence)

1. `PathProvider` (platform adapter) returns **candidates per purpose** (`config`, `data`, `cache`,
   `temp`, `logs`), each marked durable or volatile. At startup it **probes** each candidate (create
   a directory, write a file with `O_NOFOLLOW`/`O_EXCL`, fsync, read back, check ownership) and picks
   the first durable one that works. It reports `persistent=false` if only volatile ones work
   (moonlight's model, made explicit).
2. Atomic writes: write to `name.tmp.<pid>` in the same directory → `fsync` → `rename` → `fsync` the
   directory.
3. Files 0600, directories 0700; logs 0640 if Dev Manager must read them (plx rationale) `[SRC]`.
4. Secrets (tokens) go into a separate `SecretStore` interface. The first implementation is a 0600 file
   in the durable data dir, documented as **not** hardware-backed. `securitykey.operation` appears in
   plx's `requiredPermissions` `[SRC]`; whether a keystore Luna API is usable by native apps is `[UNK]`.
5. Bounded LRU disk cache with a size cap; corruption → delete and refetch; cache is always disposable.
6. SQLite: `libsqlite3.so.0` exists on all firmware `[FWDB]`, but its SQLite version and compile
   options are `[UNK]`. Prefer a bundled amalgamation if a database is ever needed.
