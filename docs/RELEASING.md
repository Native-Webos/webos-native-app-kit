# Releasing

A release is a directory of packages that can be published together with everything their licences
ask for, made by one script from a committed tree.

```sh
tools/release.sh --check-reproducible [--source-url https://example.org/kit-0.1.0-src.tar.gz]
```

Output in `build/release/<version>/` (`<version>` is `project(... VERSION)` in `CMakeLists.txt`):

| File | What it is |
|---|---|
| `org.wnk.examples.*_arm.ipk` | every example, built in a fresh directory |
| `webos-native-app-kit-<version>-src.tar.gz` | the corresponding source: `git archive` of the release commit |
| `ffmpeg-<version>.tar.xz` | upstream FFmpeg source of the bundled libraries, checksum-verified (LGPL-2.1 §6) |
| `RELEASE-NOTES.md` | packages with sizes, bundled libraries and checksums; firmware loadability; TV results |
| `SHA256SUMS` | checksums of all of the above |

## What the script checks (each stops the release)

1. The working tree is clean, so the source archive is exactly what was built.
2. `tools/test.sh host-debug` and the packaging tool tests (skip with `--skip-tests` only to
   re-run packaging after a release that already passed them).
3. `docs/compatibility/records.json` validates.
4. Every package passes `tools/check-package.py` as it is made: layout, metadata, ARM ABI, bundling
   policy, reviewed `DT_NEEDED`, and `licenses/` (GPL text, notices, `SOURCE.txt`, the licence of every
   bundled library, the FFmpeg tarball named when FFmpeg ships).
5. `tools/fw-check.py` for every package from webOS 5.3.1: every library and symbol it needs exists in
   each inventoried firmware.
6. With `--check-reproducible`: a second build in another directory gives byte-identical packages.

Nothing is tagged or pushed. The script prints the `git tag` command to run once the release is
accepted.

## What a person still does

1. **Version numbers.** Raise `project(... VERSION)` for the kit, and each example's `VERSION` in its
   `CMakeLists.txt` that changed since the last release (LG's installer will not replace a package
   with the same version).
2. **On a TV** (`docs/compatibility/records.json` needs a record for anything claimed):
   install each package (`tools/install.sh`), launch it (`tools/launch.sh`), and for the examples
   check what their README rows promise: A shows its grid and key log, B navigates and opens details,
   C loads the catalogue, D plays a bundled clip and a stream. Add a record for anything new.
3. **Read `RELEASE-NOTES.md`.** A package that fails fw-check on a release you support is a blocker.
4. **Tag** with the printed command, then publish the whole directory, source and FFmpeg tarball
   included.

## When you distribute an app built on the kit

The app is GPL-3.0-or-later (`LICENSING.md`). Publish its complete source and pass
`--source-url` (or `-DWNK_SOURCE_URL=` to your own build) so each package's `licenses/SOURCE.txt`
names it. If the app bundles FFmpeg, publish the FFmpeg tarball named in `SOURCE.txt` next to it.
