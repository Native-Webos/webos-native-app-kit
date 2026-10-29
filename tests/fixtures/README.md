# Test fixtures

`dovi/*.bin`: Dolby Vision RPUs from quietvoid's dovi_tool (`assets/tests/`, commit
`41dae646193c12de152333ff5dbc33579cb9ef4a`), MIT, Copyright (c) 2026 quietvoid. Each is a 4-byte start code
and the RPU payload. `fel_to_mel.bin` converted with mode 2 is `fel_to_81.bin`, and `mel_orig.bin` is
`mel_to_81.bin`, byte for byte (dovi_tool's `fel_conversions` and `mel_conversions` tests). Used by
`tests/unit/test_dovi.cpp`.

`subtitles/side.srt`: two SubRip cues written for the kit, used by `e2e.player.subtitle_file_beside_media`.
