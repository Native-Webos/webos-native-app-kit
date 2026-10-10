# Contributing

## Commit messages

A commit message explains **what changed and why**, for someone reading `git log` a year from now who
has never seen the planning documents.

### Rules

1. **Subject line**: imperative mood, ≤ 72 characters, no trailing period. Say what the commit adds
   or changes: "Add focus manager with modal scopes", not "Focus stuff" or "Updates".
2. **Never name a milestone, phase, ticket stage or plan step** ("M3", "milestone 2", "phase 1") in
   the subject or body. Milestones are planning vocabulary and mean nothing outside `docs/MILESTONES.md`.
3. **Body** (after one blank line, wrapped at 72 characters):
   - why the change is needed, when it is not obvious;
   - what was added or changed, as short `-` bullets naming the modules or files;
   - behaviour changes and anything a user or app developer must do differently.
4. **Verification**: a final paragraph stating how it was checked and at what level, using the evidence
   words from `docs/research/README.md` (host tests, ASan, ARM build, firmware inventory, on a named TV).
   Never claim a TV result without having run it on a TV.
5. **One logical change per commit.** Unrelated fixes go in their own commit.
6. **Trailers** last, separated by a blank line (for example `Co-Authored-By:`).

### Example

```
Add GLES2 renderer with text, clipping and letterboxing

UI code needs one drawing API that behaves the same on the host and on
the TV, without widgets touching GL.

- render/: Renderer interface, fonts via stb_truetype, glyph atlas
- render/gles2: batched rounded rects, borders, images and glyphs;
  destination alpha kept for video punch-through
- assets/fonts: Atkinson Hyperlegible (OFL)

Checked with 11 pixel tests on Mesa GLES2 and under ASan/UBSan; the
ARM build loads on webOS 5.3.1-11.2 in the firmware inventory. Ran on
an LG OLED65C1PTB (webOS 6.5.3) at about 60 fps.
```

## Before committing

Run the gates listed in `AGENTS.md` and keep `applications/**/expected-needed.txt` and
`cmake/sdl-webos-runtime-libs.txt` in sync with any dependency change.
