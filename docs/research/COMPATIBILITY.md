# Compatibility

Evidence tags: see [README.md](README.md).

## Source of the firmware data

The webosbrew firmware symbol inventory (`webosbrew-toolbox-fw-symbols`, tag
`v20260731-e1bb0c0`, from [webosbrew/dev-toolbox-cli](https://github.com/webosbrew/dev-toolbox-cli)
releases) was already extracted on this machine at `~/.cache/plxnative/fwsym/data` by an earlier
PlxNative setup. For 14 retail firmware images it lists every shared library, its `DT_NEEDED`
entries and its exported symbol names. It has no symbol versions and says nothing about runtime
behaviour.

The table below comes from [`tools/fw-lib-matrix.py`](../../tools/fw-lib-matrix.py), written for
this kit, run on 2026-10-09 `[FWDB]`. Rerun it with:

```sh
tools/fw-lib-matrix.py                     # default library list
tools/fw-lib-matrix.py --symbol libSDL2-2.0.so.0:SDL_webOSGetPanelResolution
```

| firmware | OTA id | webOS release |
|---|---|---|
| 03.23.06.01 | HE_DTV_WT1M_AFAAABAA | 1.2.0 |
| 05.06.10.01 | HE_DTV_WT1H_AFAAABAA | 1.4.0 |
| 04.06.75.01 | HE_DTV_W15M_AFAAABAA | 2.2.3 |
| 05.60.30.01 | HE_DTV_W16N_AFADABAA | 3.4.0 |
| 06.19.80.01 | HE_DTV_W17H_AFADABAA | 3.9.2 |
| 05.50.15.01 | HE_DTV_W18R_AFAAATAA | 4.4.2 |
| 05.40.20.01 | HE_DTV_W19P_AFADATAA | 4.10.0 |
| 04.30.90.01 | HE_DTV_W20L_AFAAJAAA | 5.3.1 |
| **03.40.82.01** | **HE_DTV_W21P_AFADATAA** | **6.4.0** |
| 04.40.90.01 | HE_DTV_W22O_AFABATAA | 7.4.0 |
| 03.30.60.01 | HE_DTV_W23O_AFABATAA | 8.3.0 |
| 23.20.50.01 | HE_DTV_W23O_AFABATAA | 9.2.0 |
| 33.20.40.01 | HE_DTV_W22H_AFABATPU | 10.2.0 |
| 43.21.70.01 | HE_DTV_W23O_AFABJAAA | 11.2.0 |

Only **one** webOS 6 image is in the inventory. Library variation between webOS 6 point releases
and between SoC families is `[UNK]`. The model-year names often used for these releases
(6.x = 2021 "webOS 6.0", 7.x = "webOS 22", 8.x = "webOS 23", …) are `[INF]`, based on the OTA ids
(`W21P`, `W22O`, `W23O`).

## Library matrix `[FWDB]`

```
webOS release                 1.2.0   1.4.0   2.2.3   3.4.0   3.9.2   4.4.2   4.10.0  5.3.1   6.4.0   7.4.0   8.3.0   9.2.0   10.2.0  11.2.0
libc.so.6 (glibc)             2.14.1  2.14.1  2.19    2.19    2.19    2.24    2.24    2.28    2.28    2.31    2.31    (6)     (6)     (6)
libstdc++.so.6                6.0.16  6.0.16  6.0.19  6.0.19  6.0.19  6.0.22  6.0.22  6.0.25  6.0.25  6.0.28  6.0.28  6.0.29  6.0.29  6.0.32
libSDL2-2.0.so.0              0.0.0   0.0.0   0.2.1   0.2.1   0.4.0   0.4.1   0.4.1   0.10.0  0.10.0  0.14.0  0.14.0  0.14.0  0.14.0  0.14.0
libEGL.so.1                   1.4     1.4     1.4     1.4     1.4     1.4     1.5     1.5     1.4.0   1.4.0   1.4.0   1.4.0   1.0.0   1.4.0
libGLESv2.so.2                2.0     2.0     2.0     2.0     2.0     2.0     2.0     2.0     2.1.0   2.1.0   2.1.0   2.1.0   2.0.0   2.1.0
libwayland-webos-client.so.1  1.0.0   (all releases)
libNDL_directmedia.so.1       -       -       -       -       1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   1.0.0
libplayerAPIs.so.1            1.0.0   (all releases)
libAcbAPI.so.1                -       -       1.0.0   1.0.0   1.0.0   1.0.0   1.0.0   -       -       -       -       -       -       -
libcurl.so.4                  -       -       -       -       -       -       -       4.5.0   4.5.0   4.6.0   4.6.0   4.7.0   4.7.0   4.8.0
libssl.so.1.0.0               1.0.0   ...     1.0.0 (to 4.10.0)  -   (5.3.1 has libssl.so.1.0.2 instead)
libssl.so.1.1                 -       -       -       -       -       -       -       -       1.1     1.1     1.1     -       -       -
libssl.so.3                   -       -       -       -       -       -       -       -       -       -       -       3       3       3
libpng16.so.16                present in all
libjpeg.so.8 / .62            .so.8 up to 3.9.2; .so.62 from 4.4.2
libfreetype.so.6              present in all (6.10.0 → 6.20.1)
libharfbuzz.so.0              from 1.4.0; 0.10800 on 5.x/6.x
libz.so.1                     present in all
libsqlite3.so.0               present in all
libpbnjson_c.so.2, libluna-service2.so.3, libPmLogLib.so.3, libhelpers.so.2   present in all
libpulse.so.0, libasound.so.2 present in all
libopus.so.0                  from 5.3.1
```

The "(6)" glibc entries are the SONAME suffix the script printed for releases 9.2.0 and later.
Those releases name the loader differently. The exact glibc version there is `[UNK]` from this
summary but can be read from the inventory if needed.

## Symbol spot checks `[FWDB]`

```
libSDL2-2.0.so.0:SDL_webOSGetPanelResolution   NO up to 3.9.2; yes from 4.4.2
libSDL2-2.0.so.0:SDL_webOSCursorVisibility     NO up to 3.9.2; yes from 4.4.2
libSDL2-2.0.so.0:SDL_webOSCreateExportedWindow NO up to 4.10.0; yes from 5.3.1
libSDL2-2.0.so.0:SDL_RenderGeometry            NO in every firmware (system SDL ≤ 2.0.14)
libNDL_directmedia.so.1:NDL_DirectMediaInit    yes from 3.9.2
libNDL_directmedia.so.1:NDL_DirectMediaLoad    yes from 5.3.1 (NDL v2)
libcurl.so.4:curl_easy_init                    yes from 5.3.1
libhelpers.so.2:HLunaServiceCall               yes in all
```

## What this means for webOS 6 (the key target)

| Fact | Consequence |
|---|---|
| System SDL is **2.0.10** `[FWDB]`; the NDK sysroot headers are **2.24.1** `[BUILD]` | Code built against NDK headers can reference SDL functions the TV does not have, and only the loader finds out. Either bundle a known SDL or verify imports against the inventory. |
| glibc 2.28 on the TV, **2.12.2** in the NDK sysroot `[BUILD]` | Binaries built with the NDK need only old glibc symbols. The smoke test needed only `GLIBC_2.4` `[BUILD]`. Some newer libc functions are missing at link time; punktfunk shims `getauxval`, `gettid`, `sendmmsg` `[SRC]`. |
| libstdc++ 6.0.25 (GCC 8 era) on the TV; NDK GCC 12.2 (`libstdc++.so.6.0.30`) | C++ must use `-static-libstdc++ -static-libgcc` `[BUILD]`, or the binary needs `GLIBCXX` versions the TV lacks `[INF]`. |
| `libAcbAPI` absent, `SDL_webOSCreateExportedWindow` present | Starfish video placement on webOS 6 goes through an SDL exported window `[FWDB]`, `[SRC]` (plx, samples). |
| NDL v2 present (`NDL_DirectMediaLoad`) | NDL DirectMedia v2 is a candidate media backend; hardware-confirmed upstream on webOS 6.5.2 `[UPSTREAM-HW]`. |
| libcurl.so.4 present (4.5.0) with OpenSSL 1.1 | A system HTTP stack exists on webOS 5.3.1+. On 9.x+ curl moves to OpenSSL 3, behind the same `libcurl.so.4` SONAME `[FWDB]`. |

## Device facts that need hardware `[UNK]`

- Real key codes for Play/Pause/Stop/FF/RW/colour keys **through the SDL we ship**. Codes measured
  through the system SDL 2.0.4 on webOS 4.5 (plx) need not match bundled SDL 2.30.
- Whether HOME/LIVE ever reach a native app.
- Panel resolution reported on 4K sets with each SDL; refresh rate (punktfunk saw `refresh_rate=0`).
- Writable persistent locations under each jail profile on webOS 6.
- NDL v2 PTS unit (ms vs µs).
- CA trust store contents on webOS 6 (PlxNative ships extra Let's Encrypt roots for older sets).
- Memory budget. PlxNative declares `"requiredMemory": 160` in `appinfo.json` `[SRC]`; how SAM
  enforces it is `[UNK]`.

## Model / SoC caveats reported upstream `[SRC]` (claims, not verified here)

- PlxNative README: some 2019 sets on `k5lp`/`k3lp` under Developer Mode cannot play video because
  the jail withholds a device (`/dev/rtkmem` per punktfunk `device.rs`). 2018 sets reporting
  platform release 3.9.3 never start video. A crash on `k6hp`.
- punktfunk: `hid-playstation` controller feedback only on webOS 10+. Pads type as remote keys on
  webOS 23+.
