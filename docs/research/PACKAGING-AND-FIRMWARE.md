# Packaging (IPK) and use of TV firmware — analysis and plan

Evidence tags: see [README.md](README.md). Researched 2026-10-11 from the local clones
(`plx-native`, `moonlight-tv`, `client-webos`), shallow clones of lgnome, commons-c, SDL-webOS,
dev-toolbox-cli and samples, and the webosbrew.org guides. Nothing here was run on a TV.

---

## Part A — How each project builds its package

### A.1 Pipelines side by side `[SRC]`

| Step | PlxNative | moonlight-tv | punktfunk client-webos | lgnome | webosbrew/samples |
|---|---|---|---|---|---|
| Build | Makefile + cargo, NDK | CMake + NDK toolchain file | cargo + Taskfile (Docker, arm64 host) | CMake + Docker | CMake |
| Stage | `ipkroot/data/usr/palm/applications/<id>/` (Makefile `STAGE`) | `cmake --install` into the CPack temp dir | `mktemp -d` with `bin/`, `lib/`, assets (`taskfiles/toolchain.yml` `stage-and-package`) | `cmake --install --prefix stage` | `cmake -E copy` into `staging-<target>/` |
| Strip | `strip --strip-unneeded` on the **staged copy only**; unstripped kept for crash symbolisation | `ArchiveDebugSymbols.cmake`: tar the unstripped files to `_dbgsym.tar.gz`, then strip | NDK `strip` on binary + bundled libs | — | — |
| Library tidy | — | `CleanupNameLink.cmake`: delete symlinks, rename each `.so` to its SONAME | `cp -L` (dereference) to the SONAME | — | `cmake -E copy` dereferences; *"ipk archives cannot carry"* symlinks |
| Archive | **own writer `ci/mkipk.py`** (reproducible) | `ares-package <stage> -o dist -e include -e cmake … --force-arch arm` | `ares-package` from **ares-cli-rs** (`cargo install … webosbrew/ares-cli-rs`) | `ares-package` (node `@webos-tools/cli` 3.2.4) | `ares-package --force-arch` |
| Checks after packaging | `ci/check-package.py`, `ci/check-packaged-elf.py`, `ci/expected-dt-needed.txt`, link evidence, `tools/fwcompat.py` | `webosbrew-ipk-verify -S -d` target; CI `webosbrew-ipk-verify -f markdown -d` | Homebrew manifest schema validation in CI | `--verify-ipk`: unpack with `ar x`, check layout, RUNPATH `$ORIGIN/../lib`, **no firmware NDL inside**, no forbidden codec | `<target>-verify` → `webosbrew-ipk-verify -S -d -r "<range>"` |
| Manifest | `webosbrew-package.yml` kept in repo; manifest made in release CI | `webosbrew-gen-manifest -p <ipk> -o <id>.manifest.json -i <icon> -l <link>` | `webosbrew-gen-manifest`, then `jq` to set `ipkUrl`, then schema validation | — | — |
| Install (dev) | `scp` the ipk, then `luna-send -i luna://com.webos.appInstallService/dev/install {"id","ipkUrl":"/tmp/x.ipk","subscribe":true}` (rooted) | `ares-install`, `ares-launch` (CMake targets) | `ares-setup-device`, `ares-install`, `ares-launch` | `ares-install`, `ares-launch --close/--running` | `ares-install` |
| Fast loop | `make deploy` = scp binary into the already-installed app dir (rooted) | — | — | — | — |

### A.2 What is inside an `.ipk`

An ipk is an `ar` archive with three members, in this order: `debian-binary` (`2.0\n`),
`control.tar.gz`, `data.tar.gz` `[SRC]` (plx `mkipk.py`, `check-packaged-elf.py`).

```
control.tar.gz:  ./control
                   Package: <app id>
                   Version: <x.y.z>            (three integers; must equal appinfo.json version)
                   Architecture: arm
                   Installed-Size: <KiB>       (plx: ares-package writes bytes; opkg expects KiB)
                   Maintainer, Description, Section, Priority …
                   webOS-Package-Format-Version: 2
                   webOS-Packager-Version: …
data.tar.gz:     usr/palm/applications/<id>/appinfo.json, <main executable>, icons, lib/…, assets
                 usr/palm/packages/<id>/packageinfo.json     ← required by appInstallService
                 usr/palm/services/<service id>/services.json + executable   (optional native service)
```

Facts reported by PlxNative (`ci/mkipk.py` docstring, `[SRC]` claims, its author tested them on a TV):

- **`packageinfo.json` is required.** Without it an install registers nothing, and
  `webosbrew-ipk-verify` fails with a confusing "Failed to open <ipk>".
- **GNU `ar` breaks installs.** It writes member names with a trailing `/` (`debian-binary/`), and
  `appinstalld` rejects the package with `error_code -5 "Failed to extract package"`.
  `ares-package` and `dpkg-deb` write bare names.
- **`ares-package` (node 2.4.0) is not reproducible.** Same input, different sha256. PlxNative
  normalises uid/gid/mtime/mode/order and the gzip header, so the manifest hash can be reproduced.
- The **directory name** `applications/<id>` must equal `appinfo.json` `id`. PlxNative also reads it
  at runtime to learn which install it is.
- `appinstalld` **replaces the app directory wholesale** on every install, so never keep user
  data there.

### A.3 "Repackaging" libraries: what goes into `lib/` and what never does

| Practice | Who | Why `[SRC]` |
|---|---|---|
| Bundle a newer SDL with `$ORIGIN`-relative RPATH | moonlight-tv (`lib/backports/libSDL2-2.0.so.0`, RPATH `$ORIGIN/lib/backports:$ORIGIN/../lib/backports:$ORIGIN`); punktfunk (`lib/libSDL3.so.0`, `-Wl,-rpath,$ORIGIN/../lib`) | The TV's SDL is old, varies per release, or does not exist (SDL3) |
| Fetch the bundled SDL as a **prebuilt release** or build it from a git tag | commons-c `ExternalSDL2BackportForWebOS.cmake`: `SDL2_BACKPORT_RELEASE` downloads `SDL-webOS/releases/download/<tag>/SDL2-<ver>-webos.tar.gz`; `SDL2_BACKPORT_REVISION` builds with `-DWEBOS=ON -DSDL_OFFSCREEN=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_KMSDRM=OFF …` | Reproducible, pinned |
| Copy a **sysroot** library into the package | punktfunk ships `libfreetype.so.6` from the NDK sysroot | The firmware copy is too old or different for its use (reason not documented) `[INF]` |
| Bundle your own build under a **renamed SONAME** and `dlopen` it by **absolute path** | PlxNative: `libavformat-plx.so.63`, `libavcodec-plx.so.63`, `libavutil-plx.so.61`, `libass-plx.so.0` | Avoid colliding with the TV's own FFmpeg (webOS 11 ships FFmpeg 6). Never in `DT_NEEDED`. |
| Link the C++ runtime **statically**, never bundle `libstdc++.so.6` | punktfunk (`cc-shim.sh` swaps `-lstdc++` for `libstdc++.a`) | A bundled one loaded through `DT_RPATH` shadows the TV's copy for LG libraries too. webOS 11 `libhelpers.so.2` needs `GLIBCXX_3.4.32`, which the SDK lacks: *"Failed to load webOS libraries"*. Jail env is `LD_LIBRARY_PATH=/usr/lib:$APPDIR/lib`. |
| **Never** stage firmware libraries | lgnome check: *"webOS firmware NDL libraries must not be staged"*; plx `LICENSING.md` | Proprietary, and must match the TV |
| Firmware-optional libraries are **dlopened**, not linked | plx `dynlib.rs` (SONAME candidate lists), punktfunk (NDL), lgnome (NDL) | A missing `DT_NEEDED` kills the process at `exec()`, before `main` |
| Pin the expected `DT_NEEDED` set | plx `ci/expected-dt-needed.txt` (15 SONAMEs, all present on every release 4.4.2–11.2.0) | Any new hard dependency is reviewed |
| A placeholder `libcurl` SONAME library | moonlight-tv (`commons-curl-abi-fix`) | Undocumented `[UNK]` |

---

## Part B — How the TV firmware is used

There are four separate uses. Projects mix them.

### B.1 The NDK sysroot: LG's libraries, used to **link**

- The openlgtv buildroot NDK sysroot ships headers and `.so` files with the TV's real SONAMEs:
  SDL2, GLESv2, wayland, glib, luna-service2, `libplayerAPIs`, `libpf`, `libNDL_directmedia`, … `[BUILD]`.
- Several are **link-time stubs**, so a link that succeeds proves nothing (samples, punktfunk, plx) `[SRC]`.
- PlxNative once generated its own stub `.so` files with real SONAMEs to link against FFmpeg/curl.
  It **dropped the trick**: SONAMEs move between releases (FFmpeg 55→60, curl `.so.5`→`.so.4`,
  `libAcbAPI` deleted at 5.0), which pinned the binary to one era (plx `Makefile` header) `[SRC]`.

### B.2 The firmware symbol inventory: **offline** compatibility checks

How the data is made (dev-toolbox-cli `packages/fw-extract`) `[SRC]`:

1. Download an LG OTA firmware image (`.epk`). Unpack it with **epk2extract**
   ([Informatic/epk2extract](https://github.com/Informatic/epk2extract)). webOS images are EPK v3 and
   need AES/RSA keys *"dumped from a running TV"* (epk2extract README). Output directories are
   named `rootfs.pak.unsquashfs`, `bsppart*.pak.unsquashfs`, …
2. `fw-extract -i <version>-<otaId>/ -o data/` reads `rootfs.pak.unsquashfs/etc/starfish-release`
   (webOS release, e.g. `6.4.0`), `etc/ld.so.conf` (library search path, including
   `/mnt/bsppart/...` mapped to the bsppart image) and `usr/lib/opkg/info/*.list` (owning package).
   For every `.so` it writes `<lib>.json` = `{name, package, needed[], symbols[]}` plus `index.json`
   (SONAME → file) and `info.json` (`version`, `ota_id`, `release`).
3. Published as the Debian package `webosbrew-toolbox-fw-symbols` (now **amd64 and arm64**, latest
   `v20260818-ea38b20` / 0.9.0). Installed to `/usr/share/webosbrew/compat-checker/data`.

Consumers:

| Tool | What it answers |
|---|---|
| `webosbrew-ipk-verify` (Apache-2.0) | For each firmware release: does every executable/service in the ipk, with libraries reachable through its rpath and the sibling `lib/`, resolve every `DT_NEEDED` and symbol? Exit codes 0 ok / 1 incompatible / 3 bad input / 4 no data / 6 *"built in a way no TV will install"*. Flags used upstream: `-S`, `-d`, `-r/--fw-releases "<range>"`, `-f markdown`. |
| `webosbrew-elf-verify` | The same for a bare ELF |
| `webosbrew-gen-manifest` | Homebrew manifest from an ipk |
| "Can I Use" on webosbrew.org | Per-codename library versions, built from the same data, with manual `version_override`s |
| plx `tools/fwcompat.py` (GPL) | Same question offline on a Mac; `--inventory`, `--lib … --grep`, `--release` |
| our `tools/fw-lib-matrix.py` | Library/symbol presence matrix (already in this kit) |

What it **cannot** tell you: behaviour. An exported symbol can still not work (LGNC links on webOS 5
but does not play; the NDK stubs export everything). Symbol versions are not recorded. Only 14 images
exist: one per release line, and a single webOS 6 image (6.4.0) `[FWDB]`.

Codename buckets (webosbrew `library-version` guide): goldilocks 4.0–4.4, goldilocks2 4.5–4.10,
jhericurl 5.x, **kisscurl 6.x**, mullet 7.x (webOS 22), number1 8.x (23), ombre 9.x (24),
ponytail 10.x (25), queue 11.x (26).

### B.3 Libraries **pulled from a live TV**: ABI and behaviour questions

PlxNative (`tools/abi-probe.sh`, skills `bind-tv-lib-abi` and `decompile-tv-lib`) `[SRC]`:

- `abi-probe.sh pull <soname>`: ssh to a **rooted** TV, `readlink -f /usr/lib/<soname>`, scp the real
  file into `.abi-cache/`. Then `syms` / `has` / `info` / `opts` run with the NDK binutils on the host
  (the TV has no binutils).
- `decomp.sh pull|str|syms|fn|xref`: harvests the media stack (`libplayerAPIs`, `libpf`, `libAcbAPI`, …)
  and decompiles it headless with **Ghidra**. Method: `.rodata` strings first (JSON keys are literal),
  then xrefs, then the function. Used to prove struct offsets (`MEDIA_CUSTOM_CONTENT_INFO` 0x138,
  `ptsToDecode` at +0x28), callback ABI, and which `Load` payload keys a firmware parses.
- PlxNative also found SDL's evdev→scancode table at a fixed offset in the harvested `libSDL2` (`remote-keys.md`).
- Requires root and the firmware's own binaries. Results are per-firmware evidence, not general facts.

### B.4 The firmware **at runtime**: probing instead of assuming

- `dlopen`/`dlsym` with fallbacks: ACB vs SDL exported window (`starfish.c` `vp_mode`), NDL v1 vs v2 (punktfunk) `[SRC]`.
- Read `/etc/starfish-release`, `/var/run/nyx/os_info.json`, `/etc/prefs/properties/machineName` (punktfunk `device.rs`) `[SRC]`.
- The Homebrew repo can gate an app on `requirements.webosRelease` (from `/etc/starfish-release`) and
  `requirements.deviceSoC` (from `machineName`) (webosbrew publishing guide).

---

## Part C — Distribution through Homebrew Channel (webosbrew.org)

1. Publish the ipk and `<id>.manifest.json` together over HTTPS (GitHub release).
2. The manifest is produced by `webosbrew-gen-manifest`: `id`, `version`, `type`, `title` (from
   appinfo); `iconUri`, `sourceUrl`; `rootRequired` (`true|false|optional`); `ipkUrl`, `ipkSize`,
   `ipkHash.sha256`. **Regenerate for every ipk.** There is no code signing: the sha256 over HTTPS is
   the whole integrity story (plx).
3. A PR to `webosbrew/apps-repo` adds `packages/<id>.yml`. Required: `title` (≤30), `iconUri`,
   `manifestUrl` (use `releases/latest/download/…`), `category`, `pool` (`main` = open source with
   `sourceUrl`), `description`. Optional: `shortDescription` (≤80), `detailIconUri`,
   `manifestUrlBeta`, `requirements.webosRelease`, `requirements.deviceSoC`, `funding`. Unknown
   fields are rejected.
4. Rules: tested on a real TV (name the model and release); no AI-only submissions; no piracy/DRM
   circumvention; keep upstream licence notices; root apps must not write to system partitions.
   punktfunk alternatively hosts its **own repo JSON** (`repo.json` with a `packages[]` list) that
   users add in Homebrew Channel.

---

## Part D — Plan for this kit (no code written yet)

### D.1 Principles drawn from the evidence

1. Package output must be **reproducible** and installable: bare `ar` member names, normalised tar,
   `packageinfo.json`, correct `Installed-Size` in KiB, ids consistent across `control`,
   `appinfo.json`, directory and filename.
2. **Two independent packagers.** Our own deterministic writer is the default. `ares-package`
   (ares-cli-rs or node) is a cross-check, and we diff the two package trees in CI.
3. **Bundling policy as code:** an allow-list of libraries we may ship (bundled SDL-webOS, our own
   renamed builds); a deny-list (`libstdc++.so.6`, `libNDL*`, `libplayerAPIs*`, `libAcbAPI*`,
   `libpf*`, `libEGL*`, `libGLES*`, `libwayland*`, `libluna*`, `libhelpers*`, …); every bundled
   library staged as a real file under its SONAME; RPATH exactly `$ORIGIN/lib`.
4. **Pinned `DT_NEEDED`** expectation file per target, reviewed when it changes.
5. **Three verification layers**, each labelled with what it proves:
   - (a) structural: package layout, metadata agreement, ELF arch/ABI;
   - (b) loadability: imports against the firmware inventory, via `webosbrew-ipk-verify` if
     installed, plus our own matrix reader;
   - (c) behaviour: a device run, recorded in the compatibility DB.
6. **Firmware data is local evidence, never redistributed.** Tools find it, or explain how to fetch it.
7. Pulling or decompiling firmware binaries (B.3) only happens on the owner's own rooted TV, on
   explicit request. It is not part of the default workflow.

### D.2 Tooling to build (M2), in order

| # | Tool | Does | Verified by |
|---|---|---|---|
| 1 | `tools/doctor.sh` | Report NDK, CMake, ares-cli (node or rs), `webosbrew-ipk-verify`, fw-symbols data, readelf; print install hints; never installs anything | run on this host |
| 2 | `cmake/WebOSPackage.cmake` + `tools/mkipk.py` (own code) | Stage `usr/palm/applications/<id>`, generate `packageinfo.json`, `control` (KiB), deterministic `ar`/tar.gz | host unit tests: build twice → identical sha256; parse back; bare member names |
| 3 | `tools/check-package.py` | Structural checks (A.2), bundling allow/deny lists, symlinks, modes, RPATH, `DT_NEEDED` vs expectation file | unit tests on good and bad fixture ipks |
| 4 | `tools/verify.sh` | Run `webosbrew-ipk-verify -r '>=5.3.1'` if present; always run our inventory import check; print what each proves | host, against a real built ipk |
| 5 | `tools/install.sh` / `launch.sh` / `logs.sh` | `ares-install`/`ares-launch` with `ARES_DEVICE`; optional rooted `luna-send …/dev/install` path | needs TV |
| 6 | `tools/gen-manifest.sh` (later, M10) | `webosbrew-gen-manifest` + schema check + `packages/<id>.yml` template | host |

### D.3 Experiments that need a TV (record results in the compatibility DB)

1. Install our own-writer ipk and the `ares-package` ipk: do both install and register?
2. Bundled SDL-webOS through `$ORIGIN/lib`: does it load on webOS 6? Check `LD_LIBRARY_PATH` inside the jail.
3. Dev Mode vs Homebrew install path and which directories are writable.
4. Reinstall/update: confirm the app directory is wiped.

### D.4 Decisions for the owner

1. **ares-cli flavour**: node `@webos-tools/cli` (official, npm global install) or webosbrew
   **ares-cli-rs** (amd64 `.deb`/tarball, no Node)? Recommendation: ares-cli-rs for packaging
   cross-checks and install/launch; our own writer for release artifacts.
2. **Install webosbrew dev-toolbox debs** (`ipk-verify`, `fw-symbols`, `gen-manifest`; amd64) with
   `sudo dpkg -i`? Recommended. The local fw data (`~/.cache/plxnative`, tag v20260731) is older
   than the current release.
3. **Bundled SDL source**: download the prebuilt `SDL2-2.30.12-webos.tar.gz` release (fast, what
   moonlight-tv does) or build from the tag with our NDK (reproducible, auditable)? Recommendation:
   build from tag, with the prebuilt as a fallback.
4. **Rooted TV?** It decides whether B.3 (pulling libraries) and the scp fast loop are available.

---

## Part E — Owner decision (2026-10-11): follow PlxNative's design on a NON-rooted TV

The owner prefers PlxNative's approach and has a **Developer Mode (unrooted)** TV. What we keep, and
what changes because there is no root:

### E.1 PlxNative practices we adopt (reused or adapted with attribution; licence per E.4)

| PlxNative practice | Kit equivalent |
|---|---|
| Own deterministic ipk writer (`ci/mkipk.py`): bare `ar` names, normalised tar, `packageinfo.json`, `Installed-Size` in KiB | `tools/mkipk.py` |
| Single source of version/id (`appinfo.json`) asserted against `control`, directory, filename (`ci/check-package.py`) | `tools/check-package.py` |
| Pinned `DT_NEEDED` list (`ci/expected-dt-needed.txt`) + per-ELF check of the packaged files (`check-packaged-elf.py`) | `packaging/expected-dt-needed.txt` + checker |
| Offline grading against firmware inventories (`tools/fwcompat.py`) | `tools/fw-lib-matrix.py` (exists) → import checker + `webosbrew-ipk-verify` |
| Firmware-variable libraries via `dlopen` candidate lists (`dynlib.rs`), never `DT_NEEDED` | `platform/webos/dynlib` |
| Own pinned builds under renamed SONAMEs, loaded by absolute path (`libavformat-plx.so.63`) | same pattern for any optional bundled lib, e.g. FFmpeg in M7 |
| Strip only the staged copy; keep the unstripped binary + `--build-id` for crash symbolisation | same |
| Flavours: `debug` id beside `stable` id, so testing never touches the user install (`ci/flavor.py`) | `<id>.debug` flavour as the default build |
| Event/crash/stderr logs in the runtime dir, `O_NOFOLLOW`, mode 0640 so Dev Manager can read them | logging sink |
| Path resolver from `/proc/self/exe`, durable-path candidates (`paths.rs`) | `PathProvider` with probing |

### E.2 What does NOT work without root, and the replacement

| Rooted PlxNative workflow | Why it fails unrooted `[SRC]` (plx `distribution.md` §3.6) | Our replacement |
|---|---|---|
| `make deploy`: scp binary into the app dir as root (port 22) | Dev Mode offers only `prisoner` on **port 9922**; app dir is `root:root 775` | Full ipk install every iteration: `ares-install` (ares-cli-rs) → `ares-launch`. Keep packages small and the loop scripted. |
| `luna-send …/appInstallService/dev/install` over root ssh | needs root shell | `ares-install` (uses the Dev Mode ssh key) |
| `abi-probe.sh pull`, `decompile-tv-lib` (copy `/usr/lib/*.so` off the TV) | root ssh/scp | Firmware **inventory** (offline, covers symbols and `DT_NEEDED`). Whether `prisoner` can read `/usr/lib` inside the Dev Mode jail is `[UNK]`: experiment E.3-3. |
| Reading logs via ssh `cat` as root | — | Dev Manager download, or `ares-pull`/ssh as `prisoner` if the files are group-readable `[UNK]` until tested |
| `/tmp` trigger files and the remote-key FIFO to drive the app in tests | written over root ssh | Optional dev-only HTTP/FIFO trigger in debug builds, *if* `prisoner` can write the runtime dir `[UNK]`. Otherwise drive keys through the TV's network remote API (LG second-screen/SSAP pointer-input socket, used by home-automation libraries) `[INF]`, to verify. |
| Crash reproduction with gdb | root | Crash log + build-id + offline `addr2line` on the unstripped binary |

Other Developer Mode costs (plx `distribution.md`, LG docs it quotes): the session lasts about 1000 h
and removes Dev Mode apps on expiry; renew via Dev Manager's automatic renewal. Ten reboots without
network also end it. One LG developer account per TV.

### E.3 First device experiments (need only Dev Mode)

1. `ares-setup-device` with the TV's passphrase/key; `ares-install` + `ares-launch` a minimal ipk built
   by **our** writer. Confirms package format, `packageinfo.json`, launch.
2. App writes a probe report (uid/gid, `$HOME`, `/proc/self/exe`, writable/durable path candidates,
   `/etc/starfish-release`, `machineName`, `LD_LIBRARY_PATH`, SDL/GL strings, bundled SDL loaded or
   not) to its runtime log. We fetch it with Dev Manager or `prisoner` ssh.
3. As `prisoner`: `ls -l /usr/lib/libSDL2* /usr/lib/libNDL*`, `cat /tmp/<log>`. Records what an
   unrooted shell can read.
4. Key-capture screen: press every remote button and record scancodes for **our** SDL build.

### E.4 Licence consequence (blocking decision)

PlxNative is **GPL-3.0-or-later**. We may follow its *design*, but copying its code (e.g.
`mkipk.py`, `check-package.py`, `starfish.c`, Rust modules) requires the kit to be
GPL-3.0-or-later too. Every app built on the kit would then also have to be GPL-compatible.

- **Option 1: kit is GPL-3.0-or-later.** We may reuse PlxNative files directly, with attribution.
  Apps built on the kit must be GPL.
- **Option 2: kit is MIT/Apache-2.0** (current ADR-013 recommendation). Same design, our own code
  written from the documented behaviour. No PlxNative code copied. Apps may use any licence.

**Decided 2026-10-11 (revised):** Option 1. The kit is **GPL-3.0-or-later**, so PlxNative files may be reused with attribution. Device tooling is **ares-cli-rs**.
