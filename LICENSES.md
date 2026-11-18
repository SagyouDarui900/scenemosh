# Third-party licenses

This project (the SceneMosh plugin) is a clean-room implementation.
It does not contain code copied from utl2glitch or from datamosh.aux2.

## doctest (test only)

- File: `vendor/doctest/doctest.h`
- License: MIT, Copyright (c) 2016-2023 Viktor Kirilov.
- The file is copied unchanged, including its license header.
- Source: https://github.com/doctest/doctest

## AviUtl2 SDK (M2, plugin side — not used by the core)

- File: `vendor/aviutl2_sdk/` (`plugin2.h`, `filter2.h`, `cache2.h`,
  `logger2.h`, `config2.h`, `input2.h`, `module2.h`, `output2.h`,
  `license.txt`, `SOURCE.md`)
- License: MIT, Copyright (c) 2025 Kenkun.
- The headers are copied unchanged, with `license.txt` intact.
- Source: https://github.com/aviutl2/aviutl2_sdk_mirror,
  commit 9497997988394f73db8d855ba3352cacc486f90d, retrieved 2026-08-27.
- `src/plugin/sdk.hpp` only adds `<cstdint>` / `<windows.h>` includes
  before the SDK headers (the SDK uses `int64_t` without including
  `<cstdint>`); it does not modify the SDK itself.

## FFglitch (runtime dependency, never bundled)

- License: GPL (FFmpeg-derived).
- The plugin downloads the official release from https://ffglitch.org
  and calls ffgac / ffedit as external processes. No FFglitch binary or
  source is included in this repository.

## FFmpeg (optional, for the libx264 MP4 export)

- License: GPL or LGPL, depending on the binary.
- ffmpeg.exe is never bundled; the user supplies it.
