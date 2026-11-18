# Build instructions — SceneMosh plugin (M1 core + M2 plugin + M3 panel + M4 modes)

## What this builds

- M1: `src/core/` (platform-independent C++20) and its doctest suite.
- M2: `src/plugin/` (AviUtl2 SDK) as `scenemosh.aux2`, a generic
  plugin that registers the "SceneMosh" media object. The DLL is only
  built when the target system is Windows (MSVC, or MinGW-w64 cross
  compile from Linux).
- M3: cut detection and auto-placement in the same DLL: a "SceneMosh"
  panel window (Win32 child window, registered with the host) with
  [カット点を検出], a segment list, [選択した区間を生成],
  [結果を自動配置], and [キャッシュクリア]. See "Using the SceneMosh
  panel (M3)" below.
- M4: 12 extra modes (Zoom, Shear, Shift, Slice, Smear, Vibrate,
  Mirror, Shuffle, Buffer, Delay, Stop, Sink) in the preset lists of
  both the media object and the panel, plus MP4 export (libx264)
  wired to both. See "M4: extra modes and MP4 export" below.

A prebuilt MinGW cross-compile DLL is in the project root as
`scenemosh.aux2` (SHA-256
`35d496944f96cf9ee3882c5892b4742175667d57dde3ece2b7999143a79ef52d`,
3.3 MB). It is a link check build, not an MSVC build; see
"Not yet verified". This build includes the rendered-size fix and
the tightened panel layout (2026-11-18, DESIGN.md section 19) and
the panel usability fixes (2026-11-18, DESIGN.md sections 22-23):
numbered steps and grouped settings, a progress bar, a [生成を中止]
(cancel) button, failure reasons in the status line, and a unified
"ffmpeg.exe の場所" file-path input in the panel and the media
object. The panel is 540x670 with a 72-high segment list and a
46-high status box, explicit 9pt "Yu Gothic UI" font, DPI-scaled
coordinates. It also includes the F1/F2 performance optimizations
(2026-11-18, DESIGN.md section 24) and cut-centred generation
(2026-11-18, DESIGN.md section 25): segments are [cut - before,
cut + after] with before/after defaulting to 30 frames, frames are
fitted to the scene size with black padding, and the media object
has its own 繋ぎ目前 / 繋ぎ目後 items. It also includes the segment
mode (2026-11-18, DESIGN.md section 26): the panel can build
segments cut-centred or object-based, chosen with radio buttons
and saved to `<app data>\scenemosh\panel_settings.txt`.

## Requirements

- CMake 3.16 or later
- A C++20 compiler:
  - Linux: g++ 11 or later (tested with g++ 13.3)
  - Windows: MSVC 2022 (x64), with the Ninja generator
  - Cross: MinGW-w64 `g++-mingw-w64-x86-64-posix` (the plain
    `gcc-mingw-w64` package only ships the C compiler)
- FFglitch 0.10.2 is only needed for the integration tests, not for
  building. At runtime the plugin downloads it on first use
  (see `src/core/downloader.*`).

## Linux build and test (core only)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_core
```

The unit tests run without FFglitch. The two integration tests
(encode + ffedit for every preset, Generator end-to-end) are skipped
unless `HYBRID_FFGLITCH_DIR` points at a directory containing
`ffgac` / `ffedit`:

```sh
# Linux FFglitch release, for local verification only:
curl -sL -o /tmp/ffglitch-linux.zip \
  https://ffglitch.org/pub/bin/linux64/ffglitch-0.10.2-linux-x86_64.zip
unzip -q /tmp/ffglitch-linux.zip -d /tmp/ffglitch-linux
HYBRID_FFGLITCH_DIR=/tmp/ffglitch-linux/ffglitch-0.10.2-linux-x86_64 \
  ./build/test_core
```

## MinGW-w64 cross-compile check (Linux -> Windows DLL)

```sh
cmake -S . -B build-mingw --toolchain cmake/toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

This produces `build-mingw/scenemosh.aux2` (PE32+ x86-64).
It links only system DLLs (bcrypt, KERNEL32, msvcrt, WININET) — the
MinGW runtime is statically linked, so no `libwinpthread-1.dll` is
needed on the user's PC. The DLL cannot run on Linux (no AviUtl2).

## Windows build (MSVC + Ninja)

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\test_core.exe
```

The DLL is `build\scenemosh.aux2`. Notes:

- On Windows the core links `bcrypt.lib` (SHA-256) and `wininet.lib`
  (downloader). Both are linked automatically by `CMakeLists.txt`.
- The SDK headers use `int64_t` without including `<cstdint>`; the
  plugin includes `src/plugin/sdk.hpp` first, which supplies it.

## Install on a Windows PC (AviUtl2 2.1.6a or later)

1. Close AviUtl2.
2. Copy `scenemosh.aux2` to the plugin folder, e.g.
   `C:\ProgramData\aviutl2\Plugin\scenemosh.aux2`.
3. Start AviUtl2. No FFglitch setup is needed beforehand: on the
   first [生成] the plugin downloads FFglitch 0.10.2 from
   ffglitch.org, verifies its SHA-256, and extracts `ffgac.exe` /
   `ffedit.exe` into the application data folder
   (`scenemosh\ffglitch` under it). This needs internet access
   once (~40 MB). If the download fails, [生成] logs an error and
   nothing is generated.
4. Put a video clip on a timeline layer (e.g. layer 1).
5. Add a media object: in the timeline, add an object and choose
   the "SceneMosh" media object on a layer above the clip, covering
   the frames to glitch.
6. In the SceneMosh settings panel, set:
   - 参照モード: レイヤー参照
   - 参照レイヤー: the clip's layer number (shown 1-based)
   - プリセット / 強度 / seed as wanted (17 presets: the 5 base
     modes plus 12 M4 modes — ズーム, シアー, シフト, スライス,
     スメア, バイブレート, ミラー, シャッフル, バッファ, ディレイ,
     ストップ, シンク)
   - キャッシュフォルダ: empty = default (application data folder)
   - MP4書き出し: checked = also write a libx264 MP4 after
     generation; ffmpegフォルダ: the folder containing ffmpeg.exe
     (empty = search PATH). If ffmpeg.exe is not found, the export
     fails with a log error and the cache MP4 is kept.
7. Press [生成]. The first run downloads FFglitch if needed, then
   renders the referenced frames, encodes them, and applies the
   preset. Progress is written to the host log at 10% steps.
8. When the log says the generation is done, move the cursor (scrub)
   to refresh the preview: the plugin has no API to force a
   re-render, so the new cache may not appear until the preview
   refreshes. The object should then show the glitched frames.
   Before generation it shows a magenta-bordered checkerboard
   ("not generated" mark); turn off 未生成マークを表示 to leave it
   blank instead.
9. Press [グリッチのみ再適用] to re-run only ffedit after changing
   the preset, strength, or seed (the source MP4 is reused).

## Using the SceneMosh panel (M3)

The DLL also opens a "SceneMosh" panel window (registered with the
host as a window client). It is the datamosh-style workflow: detect
cuts, pick segments, generate, and auto-place the results.

The panel is laid out in numbered steps (DESIGN.md section 22):
"手順： 1 検出 → 2 生成 → 3 配置", with the settings grouped under
"1. 検出する範囲", "2. 生成する内容", and "3. 配置と出力".

1. Put video clips on a timeline layer (e.g. layer 1), forming cuts.
2. In the SceneMosh panel, set:
   - 対象レイヤー: the clips' layer number (shown 1-based)
   - 繋ぎ目前 / 繋ぎ目後: frames to include before / after each cut.
     A timeline range selection limits the scan to that range.
   - プリセット / 強度 / seed (17 presets, as in the media object;
     some modes ignore strength and/or seed, see README.md)
   - 配置先レイヤー: 0 = automatic (first free layer above the
     source layer, then the first free layer below it)
   - MP4書き出し: checked = also write a libx264 MP4 per segment;
     ffmpeg.exe の場所: the path to ffmpeg.exe (empty = search
     PATH). When export is on, the exported MP4 is what gets
     placed; the cache MP4 stays for playback.
   - 生成後に自動で配置する: checked = place each finished MP4
     automatically
   - コンソールを表示: checked = show the ffgac / ffedit / ffmpeg
     console windows while they run
3. Press [1. カット点を検出]. The list shows one segment per cut,
   e.g. `区間 1: 100-160 (カット 120) 61f [未生成]`. Detection alone
   generates nothing. If a frame range is selected on the timeline,
   only that range is scanned; otherwise the whole scene is scanned
   (up to the last frame containing an object). A layer containing a
   フレームバッファ object stops the scan with a log error. A scene
   object is treated as a single segment; its inner sections are not
   cuts.
4. Select segments in the list (multi-select) and press
   [2. 選択した区間を生成]. Each segment is generated in turn by the
   same worker as the media object. Progress shows in the panel
   ("生成状況：区間 N を生成中：素材をエンコード中 35%" plus a
   progress bar) and in the log at 10% steps. [生成を中止] stops the
   run: the running job stops at the next frame, queued jobs are
   dropped, and cancelled segments go back to 未生成. The segment's
   cache uid is `panel-s<scene>-l<layer>-<start>-<end>`, so re-running
   the same segment reuses its cache.
5. When generation finishes, the status line reports the counts
   (成功 / 失敗 / 中止) and, on failure, the first failed segment's
   reason (e.g. `区間 2：エンコード：rendered size ...`). Finished
   segments are placed automatically if 生成後に自動で配置する is
   checked (not after a cancel). Otherwise press [3. 選択した区間を
   配置する] for the selected segments (it also places segments
   whose cache file already exists). Placed objects are named
   `glitched_<preset>_<segment number>` (e.g.
   `glitched_amplify_1`). If placement fails, the MP4 path and the
   manual steps are written to the status box and the log; the file
   is kept. All objects placed in one operation are grouped into a
   single Undo entry by the host (verify this on a real machine).
6. [キャッシュクリア] deletes every cache file in the default cache
   folder.

## M4: extra modes and MP4 export

- The preset lists in the media object and the panel now have 17
  entries: MV増幅, MV固定, MVランダム, データモッシュ,
  MV差分ドリフト, ズーム, シアー, シフト, スライス, スメア,
  バイブレート, ミラー, シャッフル, バッファ, ディレイ, ストップ,
  シンク. The strength mappings are this project's own decisions
  (see DESIGN.md section 17.1); they have **not** been tuned by
  looking at real footage yet, so the visual result of each mode
  still needs checking on a real machine and adjusting.
- MP4 export is off by default. Turn on MP4書き出し and set
  ffmpeg.exe の場所 (both the media object and the panel take the
  ffmpeg.exe file path; empty = search PATH) to write a libx264
  MP4 (preset fast, crf 18) after generation. ffmpeg.exe is never
  bundled; supply it yourself. If it is missing, generation logs an
  error at the export stage and the cache MP4 is kept. When export
  is on, auto-placement places the exported MP4; playback still
  uses the cache MP4. (Before the P8 fix the media object took a
  folder instead of a file path; a folder stored in an old project
  still works — ffmpeg.exe is appended to it.)

## Verification checklist (Windows PC)

- [ ] The preset list shows all 17 modes in both the media object
      and the panel; picking one and pressing [生成] finishes.
- [ ] Each of the 12 new modes visibly changes the picture (check
      Zoom, Shear, Slice, Smear, Shuffle in particular); report
      modes whose effect looks wrong or invisible.
- [ ] With MP4書き出し on and ffmpeg.exe available, generation
      writes a `<uid>-<src>-<glitch>.export.mp4` file, and
      auto-placement places that file instead of the cache MP4.
- [ ] With MP4書き出し on and ffmpeg.exe missing, generation logs
      an export error and the cache MP4 is kept.
- [ ] AviUtl2 starts with the DLL in place, no load error.
- [ ] "FFglitch" appears in the media object list.
- [ ] The settings panel shows all items (参照モード, 参照レイヤー,
      フレームオフセット, プリセット, 強度, seed, Iフレーム抑制,
      キャッシュフォルダ, MP4書き出し, ffmpeg.exe の場所, 素通し表示,
      未生成マークを表示, 4 buttons).
- [ ] [生成] downloads FFglitch once, then finishes; the log shows
      the glitch MP4 path.
- [ ] After scrubbing, the object shows the glitched frames, and
      playback runs at normal speed (cache playback).
- [ ] Changing only the seed and pressing [グリッチのみ再適用]
      finishes noticeably faster than a full [生成].
- [ ] 素通し表示 shows the referenced layer's image unchanged.
- [ ] A second SceneMosh object has its own cache (different uid) and
      does not disturb the first object's cache.
- [ ] [このキャッシュを削除] removes only this object's files;
      [未使用キャッシュを削除] removes orphaned files.
- [ ] 画面キャプチャ mode generates from the scene framebuffer.
- [ ] A layer containing a フレームバッファ object refuses
      generation with a log error instead of crashing.
- [ ] Closing AviUtl2 while a job runs does not hang (the worker is
      joined in UninitializePlugin).
- [ ] The "SceneMosh" panel window appears (registered window client).
- [ ] Panel layout: no label, checkbox, button, or edit text is cut
      off, at 100%, 125%, and 150% display scaling. In particular:
      "対象レイヤー", "繋ぎ目前", "プリセット", "MP4書き出し" +
      ffmpeg.exe の場所, "生成後に自動で配置する" / "コンソールを表示",
      the numbered buttons, the group titles, the segment list, and
      the status box (wraps to multiple lines). See DESIGN.md
      sections 18 and 23.
- [ ] Panel layout (grouped, DESIGN.md sections 22-23): the panel is
      540x670 with three numbered group boxes ("1. 検出する範囲",
      "2. 生成する内容", "3. 配置と出力"), a step heading, a
      progress bar, and the segment list showing about 3 entries.
      No text is cut off (no regression from the section 18 fix).
- [ ] Panel usability (DESIGN.md section 22): the step order is
      readable from the heading, group titles, and numbered buttons;
      "生成後に自動で配置する" (checkbox) and "3. 選択した区間を
      配置する" (button) are clearly different; the progress bar
      and "生成状況" line move during generation; [生成を中止]
      stops a run and the status reports 成功 / 失敗 / 中止 counts;
      a failed run shows the first failed segment's reason in the
      status line; the ffmpeg input means the same thing (an
      ffmpeg.exe file path) in the panel and the media object.
- [ ] [生成を中止] during a long generation: the run stops, the
      status reports the cancelled segments, cancelled segments go
      back to 未生成 and can be generated again, and no partial
      ".part" file is renamed into the cache.
- [ ] Generation succeeds on a layer whose clips have a different
      resolution from the scene (e.g. BB素材 clips); the log shows
      `using rendered size <w>x<h> instead of scene size <w>x<h>`.
      The generated MP4 has the clip's resolution, and the
      auto-placed MP4 appears at the same size and position as the
      original clip. See DESIGN.md section 19.
- [ ] Generation on a clip with the same resolution as the scene
      still uses the scene size (the probe does not change it).
- [ ] A clip whose rendered size changes from frame to frame
      (e.g. animated scale) fails generation partway, and the log
      shows both the rendered size and the expected size.
- [ ] [カット点を検出] lists one segment per cut on the target layer,
      with the right start / end / cut frames; a timeline range
      selection limits the scan to that range.
- [ ] Selecting segments and pressing [選択した区間を生成] generates
      only those segments; the list shows 生成中 / 生成済み / 失敗.
- [ ] With 生成後に自動で配置する checked, finished MP4s are placed on a
      free layer above the source layer, named
      `glitched_<preset>_<n>`; [3. 選択した区間を配置する] places already
      generated segments without re-generating.
- [ ] Placing several segments in one operation is a single Undo
      entry (Ctrl+Z removes all of them at once).
- [ ] A scene object is detected as one segment (its inner sections
      are not listed as cuts).
- [ ] A layer containing a フレームバッファ object stops detection
      with an error instead of scanning on.
- [ ] コンソールを表示 shows the ffgac / ffedit console windows
      while generating; unchecked, no console window appears.
- [ ] [キャッシュクリア] in the panel deletes the cache files.

## Not yet verified

- M4 (extra modes, MP4 export) is also unverified in AviUtl2:
  - The 12 new modes appear in both preset lists and generate.
  - The strength mappings look right on real footage; they were
    decided without seeing any, so some will need adjusting.
  - MP4書き出し writes the export file and auto-placement places
    it; with ffmpeg.exe missing, generation fails at the export
    stage with an error and keeps the cache MP4.
  - The panel ffmpeg path edit resolves ffmpeg.exe (empty = PATH).
- MSVC build (only MinGW cross-compile link has been checked).
- Any run inside AviUtl2. In particular, these SDK behaviours are
  assumptions from the headers, not observed facts:
  - Button callbacks resolve their object via `get_focus_object()`
    (falling back to the first selected object).
  - `FILTER_ITEM_DATA` ("データ") persists the uid across
    save/load; if it does not, each launch gets a new uid and old
    cache files become orphans.
  - `rendering_object_video` / `rendering_scene_video` can be
    called from the generator worker thread, and an object handle
    resolved in a read section stays valid afterwards.
  - `rendering_object_video`'s frame argument is the scene frame
    number (not the object-local frame).
  - `set_image_data` is the right way to show a cache frame; the
    design doc originally said `set_image_resource_data("object")`.
  - `get_video_file_cache` returns RGBA (BGRA is converted; other
    formats fall back to the marker).
  - `detect_tool_version` (running ffgac/ffedit with no arguments
    to read the banner) works on Windows as it does on Linux.
  - Two SceneMosh objects do not mix up each other's item values.
- The framebuffer guard depends on the effect being named exactly
  "フレームバッファ"; that name is from the design doc, not from an
  observed object.
- M3 (panel, cut scan, auto-placement) is also unverified in AviUtl2:
  - The panel window (Win32 child + `register_window_client`) actually
    appears and stays usable.
  - A scene object's first effect is really named "シーン".
  - `find_object(layer, frame)` returns an object that starts before
    the frame (covers it); the scan may miss an object that starts
    before the scan range.
  - `call_edit_section` groups all placements in one call into one
    Undo entry, and `create_object_from_media_file` results are
    undoable at all.
  - The free-layer search finds the layer above the source layer as
    intended (smaller layer number = higher on the timeline).
  - `call_edit_section` called from the panel (UI thread) runs its
    callback on the main thread as documented.

## Layout

```
CMakeLists.txt          core library + test executable + .aux2 DLL
cmake/toolchain-mingw64.cmake   MinGW-w64 cross-compile toolchain
scenemosh.aux2    prebuilt MinGW DLL (M3, link check only)
src/core/               log, settings, hash, presets, cut_detector,
                        cache_index, file_lock, process, ffglitch,
                        sha256, downloader, encoder, mp4_exporter,
                        generator
src/plugin/             context, cut_scan, items, panel, plugin, render,
                        sdk_logger, sdk_source_reader, timeline_placer,
                        util, sdk.hpp
tests/test_core.cpp     doctest suite (unit + gated integration tests)
vendor/aviutl2_sdk/     SDK headers + license.txt (MIT)
vendor/doctest/         doctest.h (MIT, copied unchanged)
DESIGN.md               design document (v0.1, M1+M2+M3+M4 done)
README_BUILD.md         this file
LICENSES.md             third-party license notes
```
