# Research reports — index and evidence legend

Research for the universal native LG webOS starter kit (Milestones 0 and 1 of
`../../../MASTER-MISSION.md`). Started 2026-10-09.

| Report | Covers |
|---|---|
| [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md) | The studied projects side by side |
| [WEBOS-PLATFORM.md](WEBOS-PLATFORM.md) | Process model, jail, paths, Luna, platform libraries |
| [TOOLCHAIN-AND-BUILD.md](TOOLCHAIN-AND-BUILD.md) | NDK, ABI, libstdc++/glibc, packaging, symbol verification |
| [NATIVE-RENDERING.md](NATIVE-RENDERING.md) | SDL, EGL/GLES2, video plane, text, images |
| [REMOTE-INPUT.md](REMOTE-INPUT.md) | Key delivery, access policy, scancodes, pointer, focus |
| [NATIVE-MEDIA.md](NATIVE-MEDIA.md) | NDL, Starfish/SMP, ACB, exported windows, audio |
| [NETWORKING-AND-STORAGE.md](NETWORKING-AND-STORAGE.md) | curl/TLS, CA roots, writable paths, persistence |
| [APPLICATION-LIFECYCLE.md](APPLICATION-LIFECYCLE.md) | Launch, SAM registration, background/foreground, termination |
| [DEBUGGING-AND-TESTING.md](DEBUGGING-AND-TESTING.md) | Logs, crash capture, host simulation, device loop |
| [COMPATIBILITY.md](COMPATIBILITY.md) | Firmware library matrix (14 firmwares), version notes |
| [PACKAGING-AND-FIRMWARE.md](PACKAGING-AND-FIRMWARE.md) | IPK anatomy, library bundling, firmware inventory/extraction, distribution, tooling plan |
| [LICENSING.md](LICENSING.md) | What may be reused and what may only be studied |
| [ARCHITECTURE-DECISIONS.md](ARCHITECTURE-DECISIONS.md) | Decisions taken from the evidence above |

The architecture proposal is in [`../architecture/PROPOSAL.md`](../architecture/PROPOSAL.md) and the
milestone plan in [`../MILESTONES.md`](../MILESTONES.md).

## Evidence legend

Every factual claim carries one of these tags. **No claim in these reports is tagged `[HW]`**:
no TV was used during this research.

| Tag | Meaning |
|---|---|
| `[SRC]` | Verified by reading source code, headers or build scripts (link given). |
| `[FWDB]` | Verified in the webosbrew firmware symbol inventory (14 retail firmware images; see COMPATIBILITY.md). Shows that a library or symbol **exists**, not that it **works**. |
| `[BUILD]` | Verified by compiling here with the installed NDK. |
| `[TEST]` | Verified by an automated test run here. |
| `[HW]` | Verified on physical hardware by this project. |
| `[UPSTREAM-HW]` | An upstream project says it verified this on hardware. We did not repeat it. |
| `[INF]` | Inferred from evidence, not verified. |
| `[UNK]` | Unknown; needs investigation. |

## Pinned upstream revisions

All links point at these exact commits.

| Project | Revision | Licence |
|---|---|---|
| [GLinnik21/plx-native](https://github.com/GLinnik21/plx-native) | `c8553f02102c0060601a748a48063d3e7fb81d4c` | GPL-3.0-or-later |
| [mariotaku/moonlight-tv](https://github.com/mariotaku/moonlight-tv) | `cf96ecc72cdab3aae7ba0698f463b89065ae7e3b` | GPL-3.0 |
| [punktfunk/client-webos](https://github.com/punktfunk/client-webos) | `c85b6c9d4af34747815294b390e81dbcfeef5925` | MIT OR Apache-2.0 |
| [truebest/lgnome](https://github.com/truebest/lgnome) | `3882ae4ea50ead3ea53d022aa1f2a9d80396cd11` | MIT |
| [webosbrew/SDL-webOS](https://github.com/webosbrew/SDL-webOS) (tag `release-2.30.12-webos.6`) | `263629dab0c89e75f9872eed66e197174952ce02` | zlib |
| [webosbrew/samples](https://github.com/webosbrew/samples) | `e95eed3afa7896894f811b84d8e9e277e9e2b74c` | **no licence file** |
| [webosbrew/native-toolchain](https://github.com/webosbrew/native-toolchain) | `730dbd680cc319935655b1b04c2d80fcb62fdb22` | see repo |
| [webosbrew/webos-userland](https://github.com/webosbrew/webos-userland) | `00a5f8721e15d5c10030b99b2f3e73e0c527bcce` | Unlicense (public domain) |
| [webosbrew/dev-toolbox-cli](https://github.com/webosbrew/dev-toolbox-cli) | `ea38b20ced68124fa914ac8625cc072ad94e39b2` | Apache-2.0 |
| [mariotaku/ss4s](https://github.com/mariotaku/ss4s) | `dfba721b85420ccabf91dac65be73984bf1865f9` | LGPL-3.0 |
| [mariotaku/commons-c](https://github.com/mariotaku/commons-c) | `42037e903284a4c87d6251e1dc769da559d9edda` | MIT |
| [webosose/samples](https://github.com/webosose/samples) | `df5fd71410733d120315321e30909e08864f0155` | Apache-2.0 |

`plx-native`, `moonlight-tv` and `client-webos` are cloned locally next to this kit, in
`../../../`. The others were shallow-cloned into a scratch directory for reading only.
