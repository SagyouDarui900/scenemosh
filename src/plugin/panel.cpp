// panel.cpp — see panel.hpp.
//
// Layout (client area 420x700 in 96-DPI design units, all controls
// created in WM_CREATE; design doc section 22 for the UX fixes):
//   手順の見出し (1 検出 → 2 生成 → 3 配置)
//   [1. 検出する範囲] 対象レイヤー / 繋ぎ目前 / 繋ぎ目後 /
//       区間 (カット中心 / オブジェクト単位) / 説明
//   [2. 生成する内容] プリセット / 強度 / seed / 説明
//   [3. 配置と出力] 配置先レイヤー (0=自動) / [MP4書き出し]
//       ffmpeg.exe の場所 / 説明 / [生成後に自動で配置する]
//       [コンソールを表示] / 説明
//   [1. カット点を検出] [2. 選択した区間を生成] /
//   [生成を中止] [3. 選択した区間を配置する] [キャッシュクリア] /
//   生成状況の表示と進捗バー /
//   検出結果の一覧 (multi-select list box) / ステータス表示
//
// The segment mode (design doc section 26) is chosen with two radio
// buttons in group 1 and saved to <app data>\scenemosh\
// panel_settings.txt, so it survives closing and reopening the panel
// (and restarting the host). Switching the mode alone does not
// rebuild the segment list: the next [1. カット点を検出] rebuilds
// it in the new mode.
//
// Sizing (see DESIGN.md section 18): every control gets the same
// 9pt "Yu Gothic UI" font (set with WM_SETFONT), and all coordinates
// are 96-DPI design units scaled by the window's DPI. At 96 DPI,
// 9pt is a 12px em, so a full-width character is 12px wide; label
// and button widths are the text length x 12px plus padding, and the
// right edge of every control stops 12px short of the panel edge.
// The first version set no font and used fixed 96-DPI pixels, so on
// a PC with a larger default font or 125%/150% display scaling the
// labels and buttons were cut off.
//
// The panel never calls the SDK from the generator worker thread:
// the finished callback only posts WM_SCENEMOSH_JOB_DONE to the
// window, and the window procedure (UI thread) does the placement
// through call_edit_section.

#include "panel.hpp"

#include <windows.h>

#include <commctrl.h>

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "cache_index.hpp"
#include "context.hpp"
#include "cut_detector.hpp"
#include "cut_scan.hpp"
#include "hash.hpp"
#include "presets.hpp"
#include "process.hpp"
#include "sdk_logger.hpp"
#include "sdk_source_reader.hpp"
#include "settings.hpp"
#include "timeline_placer.hpp"
#include "util.hpp"

namespace hybrid::plugin {

namespace {

// Control ids.
constexpr int kIdLayerEdit = 101;
constexpr int kIdBeforeEdit = 102;
constexpr int kIdAfterEdit = 103;
constexpr int kIdPresetCombo = 104;
constexpr int kIdStrengthEdit = 105;
constexpr int kIdSeedEdit = 106;
constexpr int kIdFallbackEdit = 107;
constexpr int kIdAutoPlaceCheck = 108;
constexpr int kIdConsoleCheck = 109;
constexpr int kIdDetectButton = 110;
constexpr int kIdGenerateButton = 111;
constexpr int kIdPlaceButton = 112;
constexpr int kIdClearCacheButton = 113;
constexpr int kIdSegmentList = 114;
constexpr int kIdStatusEdit = 115;
constexpr int kIdExportCheck = 116;
constexpr int kIdFfmpegEdit = 117;
constexpr int kIdCancelButton = 118;
constexpr int kIdProgressBar = 119;
constexpr int kIdProgressLabel = 120;
constexpr int kIdModeCutRadio = 121;
constexpr int kIdModeObjectRadio = 122;
constexpr int kIdSummaryLabel = 123;
constexpr int kIdAngleLabel = 124;
constexpr int kIdAngleEdit = 125;

// Posted by a job's finished callback (worker thread) when one panel
// job ends. wParam = entry index, lParam = 1 when ok, 0 on failure.
constexpr UINT kMsgJobDone = WM_APP + 1;
// Posted by a job's progress callback (worker thread). wParam = the
// entry index; the progress itself is read from g_state.
constexpr UINT kMsgProgress = WM_APP + 2;

enum class SegmentStatus {
    NotGenerated,
    Generating,
    Done,
    Failed,
    Placed,
};

const wchar_t* status_text(SegmentStatus s) {
    switch (s) {
        case SegmentStatus::NotGenerated: return L"未生成";
        case SegmentStatus::Generating: return L"生成中";
        case SegmentStatus::Done: return L"生成済み";
        case SegmentStatus::Failed: return L"失敗";
        case SegmentStatus::Placed: return L"配置済み";
    }
    return L"";
}

// Core stage names ("encode", "glitch", "export", "done") in
// Japanese: a verb phrase for the progress line, a noun for the
// failure line.
const wchar_t* stage_verb(const std::string& stage) {
    if (stage == "encode") return L"素材をエンコード中";
    if (stage == "glitch") return L"グリッチを適用中";
    if (stage == "export") return L"MP4を書き出し中";
    if (stage == "done") return L"完了";
    return L"生成中";
}

const wchar_t* stage_noun(const std::string& stage) {
    if (stage == "encode") return L"エンコード";
    if (stage == "glitch") return L"グリッチ適用";
    if (stage == "export") return L"書き出し";
    if (stage == "done") return L"完了";
    return L"生成";
}

struct SegmentEntry {
    CutSegment segment;
    SegmentStatus status = SegmentStatus::NotGenerated;
    std::filesystem::path glitch_path; // set when Done
    // Set when Done and MP4 export was on: placement prefers this
    // file (design doc section 10); the cache MP4 stays for playback.
    std::filesystem::path export_path;
    // Set when Failed: the core stage ("encode", "glitch", "export")
    // and error text, shown in the status line (design doc P5).
    std::string error;
    std::string stage;
};

struct PanelState {
    std::mutex mutex;
    std::vector<SegmentEntry> entries;
    int pending_jobs = 0;
    int scene_id = 0;
    int target_layer = 0; // 0-based, from the last scan
    int fps_num = 30;
    int fps_den = 1;
    // Progress of the currently running job, written by the worker
    // thread's progress callback and read by the UI thread.
    int progress_entry = -1;
    double progress_fraction = 0.0;
    std::string progress_stage;
    // Jobs cancelled in the current run (counted in their finished
    // callbacks); reset when the run's final status is shown.
    int cancelled_jobs = 0;
};

HWND g_hwnd = nullptr;
PanelState g_state;

// UI font and DPI scale for the panel (see the header comment and
// DESIGN.md section 18). g_font is created once in build_controls
// and deleted in WM_DESTROY; g_font_is_stock is true only when the
// fallback stock font was used (stock objects must not be deleted).
HFONT g_font = nullptr;
bool g_font_is_stock = false;
int g_dpi = 96;

// The window's DPI. GetDpiForWindow is resolved at runtime so the
// code does not depend on the SDK headers declaring it; when it is
// unavailable (older Windows), the device context's DPI is used.
int dpi_for_window(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool looked_up = false;
    if (!looked_up) {
        looked_up = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(user32, "GetDpiForWindow"));
        }
    }
    if (fn != nullptr) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<int>(dpi);
    }
    HDC hdc = GetDC(hwnd);
    if (hdc != nullptr) {
        const int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hwnd, hdc);
        if (dpi > 0) return dpi;
    }
    return 96;
}

// Scale a 96-DPI design unit to the panel's DPI.
int scale_px(int v) { return MulDiv(v, g_dpi, 96); }

HFONT make_ui_font(int dpi) {
    HFONT font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
    if (font == nullptr) {
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_font_is_stock = true;
    }
    return font;
}

int control_int(HWND parent, int id, int fallback) {
    BOOL ok = FALSE;
    const UINT v = GetDlgItemInt(parent, id, &ok, FALSE);
    return ok ? static_cast<int>(v) : fallback;
}

bool control_checked(HWND parent, int id) {
    return IsDlgButtonChecked(parent, id) == BST_CHECKED;
}

std::wstring control_text(HWND parent, int id) {
    HWND item = GetDlgItem(parent, id);
    if (item == nullptr) return {};
    const int len = GetWindowTextLengthW(item);
    if (len <= 0) return {};
    std::wstring buf(static_cast<std::size_t>(len), L'\0');
    GetWindowTextW(item, buf.data(), len + 1);
    return buf;
}

void set_status(HWND parent, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(parent, kIdStatusEdit), text.c_str());
}

void set_status_utf8(HWND parent, const std::string& text) {
    set_status(parent, utf8_to_wide(text));
}

std::wstring entry_line(const SegmentEntry& entry, int index) {
    const std::int64_t frames = entry.segment.frame_count();
    if (entry.segment.kind == SegmentKind::Object) {
        // Object segments refer to one object; the number is the
        // object's position on the layer (1-based, in scan order).
        return format_wstring(L"区間 %d: %lld-%lld (オブジェクト %lld) %lldf [%s]",
                             index + 1,
                             static_cast<long long>(entry.segment.start_frame),
                             static_cast<long long>(entry.segment.end_frame),
                             static_cast<long long>(entry.segment.object_index + 1),
                             static_cast<long long>(frames),
                             status_text(entry.status));
    }
    return format_wstring(L"区間 %d: %lld-%lld (カット %lld) %lldf [%s]",
                         index + 1,
                         static_cast<long long>(entry.segment.start_frame),
                         static_cast<long long>(entry.segment.end_frame),
                         static_cast<long long>(entry.segment.cut_frame),
                         static_cast<long long>(frames),
                         status_text(entry.status));
}

// The segment mode is saved in <app data>\scenemosh\
// panel_settings.txt, the same app-data folder the cache and the
// FFglitch tools already use (context.cpp: app_data_dir). The panel
// has no other saved settings; the other inputs keep their initial
// values. A missing or unreadable file means the default (cut).
std::filesystem::path panel_settings_path() {
    return app_data_dir() / L"scenemosh" / L"panel_settings.txt";
}

SegmentMode load_panel_segment_mode() {
    std::ifstream in(panel_settings_path());
    if (!in) return SegmentMode::CutCentred;
    std::string line;
    std::getline(in, line);
    if (line == "segment_mode=object") return SegmentMode::ObjectBased;
    return SegmentMode::CutCentred;
}

void save_panel_segment_mode(SegmentMode mode) {
    const std::filesystem::path path = panel_settings_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        log_warning(&sdk_logger(),
                    "panel: could not save the segment mode");
        return;
    }
    out << "segment_mode="
        << (mode == SegmentMode::ObjectBased ? "object" : "cut") << '\n';
}

// The mode selected by the radio buttons in group 1.
SegmentMode panel_segment_mode(HWND parent) {
    return IsDlgButtonChecked(parent, kIdModeObjectRadio) == BST_CHECKED
               ? SegmentMode::ObjectBased
               : SegmentMode::CutCentred;
}

void update_selection_summary(HWND parent);

// Rebuild the list box from g_state.entries. UI thread only.
void refresh_list(HWND parent) {
    HWND list = GetDlgItem(parent, kIdSegmentList);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        for (std::size_t i = 0; i < g_state.entries.size(); ++i) {
            const std::wstring line = entry_line(g_state.entries[i], static_cast<int>(i));
            const LRESULT pos = SendMessageW(list, LB_ADDSTRING, 0,
                                             reinterpret_cast<LPARAM>(line.c_str()));
            if (pos >= 0) {
                SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(pos),
                             static_cast<LPARAM>(i));
            }
        }
    }
    update_selection_summary(parent);
}

std::vector<int> selected_indices(HWND parent) {
    HWND list = GetDlgItem(parent, kIdSegmentList);
    const LRESULT count = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return {};
    std::vector<int> items(static_cast<std::size_t>(count));
    const LRESULT got = SendMessageW(list, LB_GETSELITEMS,
                                     static_cast<WPARAM>(count),
                                     reinterpret_cast<LPARAM>(items.data()));
    if (got <= 0) return {};
    items.resize(static_cast<std::size_t>(got));
    std::vector<int> out;
    for (const int item : items) {
        const LRESULT data =
            SendMessageW(list, LB_GETITEMDATA, static_cast<WPARAM>(item), 0);
        if (data >= 0) out.push_back(static_cast<int>(data));
    }
    return out;
}

void update_selection_summary(HWND parent) {
    HWND label = GetDlgItem(parent, kIdSummaryLabel);
    if (label == nullptr) return;
    const std::vector<int> sel = selected_indices(parent);
    std::size_t total_count = 0;
    std::int64_t total_frames = 0;
    std::int64_t sel_frames = 0;
    double fps = 30.0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        total_count = g_state.entries.size();
        for (const auto& e : g_state.entries) {
            total_frames += e.segment.frame_count();
        }
        for (int idx : sel) {
            if (idx >= 0 && idx < static_cast<int>(g_state.entries.size())) {
                sel_frames += g_state.entries[static_cast<std::size_t>(idx)].segment.frame_count();
            }
        }
        if (g_state.fps_num > 0 && g_state.fps_den > 0) {
            fps = static_cast<double>(g_state.fps_num) / static_cast<double>(g_state.fps_den);
        }
    }
    std::wstring text;
    if (sel.empty()) {
        const double sec = (fps > 0.0) ? (static_cast<double>(total_frames) / fps) : 0.0;
        text = format_wstring(L"選択中: なし (全 %zu 区間 / 計 %lldf, 約 %.1f秒)",
                              total_count, static_cast<long long>(total_frames), sec);
    } else {
        const double sec = (fps > 0.0) ? (static_cast<double>(sel_frames) / fps) : 0.0;
        text = format_wstring(L"選択中: %zu 区間 / 計 %lldf (約 %.1f秒)  [全 %zu 区間 / %lldf]",
                              sel.size(), static_cast<long long>(sel_frames), sec,
                              total_count, static_cast<long long>(total_frames));
    }
    SetWindowTextW(label, text.c_str());
}

// Build the core Settings for one panel segment. The uid is stable
// for the same scene / layer / segment, so a re-run reuses the cache.
Settings build_panel_settings(const SegmentEntry& entry, int preset_index,
                              int strength, std::uint32_t seed, int angle,
                              const EDIT_INFO& info, bool export_mp4) {
    Settings s;
    int target_layer = 0;
    int scene_id = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        target_layer = g_state.target_layer;
        scene_id = g_state.scene_id;
    }
    s.uid = format_string("panel-s%d-l%d-%lld-%lld", scene_id, target_layer + 1,
                          static_cast<long long>(entry.segment.start_frame),
                          static_cast<long long>(entry.segment.end_frame));
    s.ref_mode = RefMode::Layer;
    s.ref_layer = target_layer;
    s.frame_offset = 0;
    s.preset = static_cast<PresetId>(preset_index);
    s.strength = strength;
    s.seed = seed;
    s.angle = angle;
    s.suppress_iframe = true;
    s.export_mp4 = export_mp4;
    s.cache_dir = resolve_cache_dir({});
    s.width = info.width;
    s.height = info.height;
    s.fps_num = info.rate;
    s.fps_den = info.scale;
    s.interval.start_frame = entry.segment.start_frame;
    s.interval.end_frame = entry.segment.end_frame;
    if (entry.segment.kind == SegmentKind::Cut) {
        s.cut_offset = entry.segment.cut_frame - entry.segment.start_frame;
    } else {
        s.cut_offset = -1;
    }
    s.normalize();
    return s;
}

int panel_preset_index(HWND parent) {
    const LRESULT sel =
        SendMessageW(GetDlgItem(parent, kIdPresetCombo), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= kPresetCount) return 0;
    return static_cast<int>(sel);
}

void update_angle_control_visibility(HWND parent) {
    const int pidx = panel_preset_index(parent);
    PresetId preset = PresetId::Amplify;
    if (preset_from_index(pidx, preset)) {
        const bool show = preset_supports_angle(preset);
        const int cmd = show ? SW_SHOW : SW_HIDE;
        HWND lbl = GetDlgItem(parent, kIdAngleLabel);
        HWND edt = GetDlgItem(parent, kIdAngleEdit);
        if (lbl != nullptr) ShowWindow(lbl, cmd);
        if (edt != nullptr) ShowWindow(edt, cmd);
    }
}

// Resolve ffmpeg.exe for the panel export step: the text in the
// ffmpeg path edit wins when set, otherwise the bare name so
// Windows searches PATH. ffmpeg.exe is never bundled.
std::filesystem::path panel_ffmpeg_exe(HWND parent) {
    const std::wstring text = control_text(parent, kIdFfmpegEdit);
    if (text.empty()) return std::filesystem::path(L"ffmpeg.exe");
    return std::filesystem::path(text);
}

// Find an already-generated cache file for this entry, if any.
// Returns false when nothing was generated yet. When MP4 export is
// on, the exported file is preferred (design doc section 10).
bool find_generated_cache(const SegmentEntry& entry, const EDIT_INFO& info,
                          HWND parent, std::filesystem::path& out_path) {
    const bool export_mp4 = control_checked(parent, kIdExportCheck);
    const Settings settings = build_panel_settings(
        entry, panel_preset_index(parent), control_int(parent, kIdStrengthEdit, 50),
        static_cast<std::uint32_t>(control_int(parent, kIdSeedEdit, 0)),
        control_int(parent, kIdAngleEdit, 0), info, export_mp4);
    Context& ctx = context();
    const std::string src_hash =
        compute_src_hash(settings, ctx.ffgac_version);
    CacheIndex index(settings.cache_dir);
    std::error_code ec;
    if (export_mp4) {
        for (int gen = 0; gen < kMaxGeneration; ++gen) {
            const std::filesystem::path p = index.export_path(
                settings.uid, src_hash,
                compute_glitch_hash(settings, ctx.ffedit_version, gen));
            if (std::filesystem::exists(p, ec)) {
                out_path = p;
                return true;
            }
        }
    }
    for (int gen = 0; gen < kMaxGeneration; ++gen) {
        const std::filesystem::path p = index.glitch_path(
            settings.uid, src_hash,
            compute_glitch_hash(settings, ctx.ffedit_version, gen), gen);
        if (std::filesystem::exists(p, ec)) {
            out_path = p;
            return true;
        }
    }
    return false;
}

void place_entries(HWND parent, const std::vector<int>& indices,
                   bool only_done) {
    Context& ctx = context();
    if (ctx.edit == nullptr) {
        set_status(parent, L"編集ハンドルがありません。");
        return;
    }
    EDIT_INFO info{};
    ctx.edit->get_edit_info(&info, sizeof(info));

    // Copy what the placement needs out of the shared state first;
    // the cache lookup below must not run while the mutex is held
    // (build_panel_settings locks it too).
    struct PendingPlace {
        int entry_index = -1;
        SegmentEntry entry;
        bool needs_cache_lookup = false;
    };
    std::vector<PendingPlace> pending;
    int target_layer = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        target_layer = g_state.target_layer;
        for (const int idx : indices) {
            if (idx < 0 || idx >= static_cast<int>(g_state.entries.size())) {
                continue;
            }
            const SegmentEntry& entry =
                g_state.entries[static_cast<std::size_t>(idx)];
            PendingPlace p;
            p.entry_index = idx;
            p.entry = entry;
            if (entry.status == SegmentStatus::Done &&
                (!entry.glitch_path.empty() || !entry.export_path.empty())) {
                p.needs_cache_lookup = false;
            } else if (!only_done) {
                p.needs_cache_lookup = true;
            } else {
                continue;
            }
            pending.push_back(std::move(p));
        }
    }
    std::vector<PlaceRequest> requests;
    std::vector<int> request_entry;
    int not_generated = 0;
    const int fallback = control_int(parent, kIdFallbackEdit, 0);
    for (const PendingPlace& p : pending) {
        std::filesystem::path file;
        if (p.needs_cache_lookup) {
            // The place button also accepts already-generated files.
            if (!find_generated_cache(p.entry, info, parent, file)) {
                ++not_generated;
                continue;
            }
        } else {
            // Prefer the exported MP4 when one was written (design doc
            // section 10); otherwise place the glitch cache MP4.
            file = !p.entry.export_path.empty() ? p.entry.export_path
                                                : p.entry.glitch_path;
        }
        PlaceRequest req;
        req.file = file;
        req.source_layer = target_layer;
        req.start_frame = static_cast<int>(p.entry.segment.start_frame);
        req.frame_count = static_cast<int>(p.entry.segment.frame_count());
        req.object_name = format_string(
            "glitched_%s_%d",
            preset_id_name(
                static_cast<PresetId>(panel_preset_index(parent))),
            p.entry_index + 1);
        req.fallback_layer = fallback > 0 ? fallback - 1 : -1;
        requests.push_back(std::move(req));
        request_entry.push_back(p.entry_index);
    }
    if (requests.empty()) {
        if (not_generated > 0) {
            set_status(parent,
                       L"未生成の区間があります。[選択した区間を生成] を先に実行してください。");
        } else {
            set_status(parent, L"配置する区間がありません。");
        }
        return;
    }
    std::vector<PlaceOutcome> outcomes;
    place_media_files(ctx.edit, requests, outcomes);
    int placed = 0;
    std::string first_error;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            if (outcomes[i].ok) {
                ++placed;
                SegmentEntry& entry =
                    g_state.entries[static_cast<std::size_t>(request_entry[i])];
                if (entry.status == SegmentStatus::Done) {
                    entry.status = SegmentStatus::Placed;
                }
            } else if (first_error.empty()) {
                first_error = outcomes[i].error;
            }
        }
    }
    refresh_list(parent);
    if (!first_error.empty()) {
        set_status_utf8(parent, first_error);
        return;
    }
    set_status(parent, format_wstring(L"%d 件を配置しました。", placed));
}

void on_detect(HWND parent) {
    Context& ctx = context();
    if (ctx.edit == nullptr) {
        set_status(parent, L"編集ハンドルがありません。");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.pending_jobs > 0) {
            set_status(parent, L"生成中は検出できません。生成が終わるまで待ってください。");
            return;
        }
    }
    const int layer = control_int(parent, kIdLayerEdit, 1) - 1;
    const int before = control_int(parent, kIdBeforeEdit, 0);
    const int after = control_int(parent, kIdAfterEdit, 0);
    if (layer < 0) {
        set_status(parent, L"対象レイヤーは 1 以上を指定してください。");
        return;
    }
    EDIT_INFO info{};
    ctx.edit->get_edit_info(&info, sizeof(info));
    int scan_start = 0;
    int scan_end = info.frame_max;
    if (info.select_range_start >= 0 && info.select_range_end >= 0) {
        scan_start = info.select_range_start;
        scan_end = info.select_range_end;
    }
    if (scan_end < scan_start) {
        set_status(parent, L"走査する範囲がありません。");
        return;
    }
    std::vector<CutSegment> segments;
    std::string error;
    // The mode is read here, at detect time: switching the radio
    // buttons alone leaves the shown list untouched until the next
    // detect rebuilds it.
    if (!scan_cut_segments(ctx.edit, layer, before, after, scan_start,
                           scan_end, segments, error,
                           panel_segment_mode(parent))) {
        set_status_utf8(parent, "検出できませんでした: " + error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.entries.clear();
        for (const CutSegment& seg : segments) {
            SegmentEntry entry;
            entry.segment = seg;
            entry.status = SegmentStatus::NotGenerated;
            g_state.entries.push_back(entry);
        }
        g_state.scene_id = info.scene_id;
        g_state.target_layer = layer;
        g_state.fps_num = info.rate > 0 ? info.rate : 30;
        g_state.fps_den = info.scale > 0 ? info.scale : 1;
    }
    refresh_list(parent);
    std::int64_t total_frames = 0;
    for (const auto& seg : segments) total_frames += seg.frame_count();
    set_status(parent, format_wstring(
                            L"%d 区間を検出しました（計 %lld フレーム）。生成する区間を選んでください。",
                            static_cast<int>(segments.size()),
                            static_cast<long long>(total_frames)));
}

// While a generation is running, [生成] and [検出] cannot be used
// (a second run would fight over the same entries) and [中止] can.
void set_generating_ui(HWND parent, bool generating) {
    EnableWindow(GetDlgItem(parent, kIdGenerateButton), generating ? FALSE : TRUE);
    EnableWindow(GetDlgItem(parent, kIdDetectButton), generating ? FALSE : TRUE);
    EnableWindow(GetDlgItem(parent, kIdCancelButton), generating ? TRUE : FALSE);
}

void set_progress_label(HWND parent, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(parent, kIdProgressLabel), text.c_str());
}

void set_progress_bar(HWND parent, int percent) {
    SendMessageW(GetDlgItem(parent, kIdProgressBar), PBM_SETPOS,
                 static_cast<WPARAM>(percent < 0 ? 0 : percent > 100 ? 100 : percent), 0);
}

void on_cancel(HWND parent) {
    cancel_generator();
    set_status(parent, L"生成を中止しています…");
    set_progress_label(parent, L"生成状況：中止しています…");
}

// Progress posted by a job (kMsgProgress): read the latest values
// from g_state and update the bar and label on the UI thread.
void on_progress(HWND parent, WPARAM wparam) {
    const int idx = static_cast<int>(wparam);
    int pct = 0;
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.progress_entry != idx) return;
        pct = static_cast<int>(g_state.progress_fraction * 100.0);
        const int total = static_cast<int>(g_state.entries.size());
        const int done_or_gen = [&] {
            int n = 0;
            for (const auto& e : g_state.entries) {
                if (e.status == SegmentStatus::Done ||
                    e.status == SegmentStatus::Placed ||
                    e.status == SegmentStatus::Failed) {
                    ++n;
                }
            }
            return n;
        }();
        // "区間 2/5 を生成中：素材をエンコード中 47%"
        text = format_wstring(L"生成状況：区間 %d/%d を生成中：%s %d%%",
                              done_or_gen + 1, total,
                              stage_verb(g_state.progress_stage),
                              pct);
    }
    set_progress_label(parent, text);
    set_progress_bar(parent, pct);
}

void on_generate(HWND parent) {
    Context& ctx = context();
    if (ctx.edit == nullptr) {
        set_status(parent, L"編集ハンドルがありません。");
        return;
    }
    const std::vector<int> indices = selected_indices(parent);
    if (indices.empty()) {
        set_status(parent, L"生成する区間を一覧から選んでください。");
        return;
    }
    if (!confirm_ffglitch_download(parent)) {
        set_status(parent, L"FFglitch のダウンロードがキャンセルされたため、生成を中止しました。");
        return;
    }
    EDIT_INFO info{};
    ctx.edit->get_edit_info(&info, sizeof(info));
    if (info.width <= 0 || info.height <= 0) {
        set_status(parent, L"シーンのサイズが分かりません。");
        return;
    }
    set_show_child_console(control_checked(parent, kIdConsoleCheck));
    const int preset_index = panel_preset_index(parent);
    const int strength = control_int(parent, kIdStrengthEdit, 50);
    const std::uint32_t seed =
        static_cast<std::uint32_t>(control_int(parent, kIdSeedEdit, 0));
    const int angle = control_int(parent, kIdAngleEdit, 0);
    const bool export_mp4 = control_checked(parent, kIdExportCheck);
    const std::filesystem::path ffmpeg_exe = panel_ffmpeg_exe(parent);
    int target_layer = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        target_layer = g_state.target_layer;
    }
    int submitted = 0;
    for (const int idx : indices) {
        SegmentEntry entry_copy;
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (idx < 0 || idx >= static_cast<int>(g_state.entries.size())) {
                continue;
            }
            entry_copy = g_state.entries[static_cast<std::size_t>(idx)];
            g_state.entries[static_cast<std::size_t>(idx)].status =
                SegmentStatus::Generating;
        }
        const Settings settings =
            build_panel_settings(entry_copy, preset_index, strength, seed, angle,
                                 info, export_mp4);
        const std::int64_t count = settings.interval.frame_count();
        auto provider = std::make_shared<LayerSourceReader>(
            ctx.edit, target_layer, settings.interval.start_frame, count,
            settings.width, settings.height, &sdk_logger());

        Job job;
        job.settings = settings;
        job.glitch_only = false;
        job.provider = provider;
        if (export_mp4) {
            job.ffmpeg_exe = ffmpeg_exe;
        }
        // Progress goes to the panel (bar + label) and to the log.
        // The callback runs on the worker thread: it only writes
        // g_state and posts a message; the UI thread does the
        // control updates. last_pct is per-job (mutable lambda).
        job.progress = [idx, hwnd = parent, last_pct = -1](
                           double progress, const std::string& stage) mutable {
            const int pct = static_cast<int>(progress * 100.0);
            {
                std::lock_guard<std::mutex> lock(g_state.mutex);
                g_state.progress_entry = idx;
                g_state.progress_fraction = progress;
                g_state.progress_stage = stage;
            }
            if (pct != last_pct) {
                last_pct = pct;
                PostMessageW(hwnd, kMsgProgress, static_cast<WPARAM>(idx), 0);
                const double stepped =
                    static_cast<double>(static_cast<int>(progress * 10.0)) /
                    10.0;
                log_info(&sdk_logger(),
                         format_string("generating %s: %d%%", stage.c_str(),
                                       static_cast<int>(stepped * 100.0)));
            }
        };
        job.finished = [idx, hwnd = parent](const JobResult& result) {
            {
                std::lock_guard<std::mutex> lock(g_state.mutex);
                if (idx >= 0 &&
                    idx < static_cast<int>(g_state.entries.size())) {
                    SegmentEntry& entry =
                        g_state.entries[static_cast<std::size_t>(idx)];
                    if (result.cancelled) {
                        // Back to "not generated" so the segment can
                        // be generated again; counted separately.
                        entry.status = SegmentStatus::NotGenerated;
                        entry.error.clear();
                        entry.stage.clear();
                        ++g_state.cancelled_jobs;
                    } else {
                        entry.status = result.ok ? SegmentStatus::Done
                                                 : SegmentStatus::Failed;
                        if (result.ok) {
                            entry.glitch_path = result.glitch_path;
                            entry.export_path = result.export_path;
                        } else {
                            entry.error = result.error;
                            entry.stage = result.stage;
                        }
                    }
                }
            }
            PostMessageW(hwnd, kMsgJobDone, static_cast<WPARAM>(idx),
                         result.ok ? 1 : 0);
            if (!result.ok && !result.cancelled) {
                log_error(&sdk_logger(), "panel generation failed at " +
                                             result.stage + ": " +
                                             result.error);
            }
        };
        ensure_tools_and_submit(std::move(job), default_tools_dir());
        ++submitted;
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            ++g_state.pending_jobs;
        }
    }
    refresh_list(parent);
    if (submitted == 0) {
        set_status(parent, L"生成できませんでした。");
        return;
    }
    set_generating_ui(parent, true);
    set_status(parent, format_wstring(L"%d 区間の生成を開始しました。",
                                     submitted));
}

void on_place(HWND parent) {
    const std::vector<int> indices = selected_indices(parent);
    if (indices.empty()) {
        set_status(parent, L"配置する区間を一覧から選んでください。");
        return;
    }
    place_entries(parent, indices, /*only_done=*/false);
}

void on_clear_cache(HWND parent) {
    const ClearCacheResult res = clear_all_cache();
    if (res.locked > 0) {
        const std::wstring msg =
            L"キャッシュを削除しました（" + std::to_wstring(res.removed) +
            L"件削除、" + std::to_wstring(res.locked) +
            L"件はAviUtl2で使用中のため保持）。";
        set_status(parent, msg);
    } else {
        const std::wstring msg =
            L"キャッシュを削除しました（" + std::to_wstring(res.removed) +
            L"件削除）。";
        set_status(parent, msg);
    }
}

void on_job_done(HWND parent, WPARAM wparam) {
    const int idx = static_cast<int>(wparam);
    int done = 0;
    int failed = 0;
    int cancelled = 0;
    int pending = 0;
    // First failed entry, for the reason line (P5).
    int failed_index = -1;
    std::string failed_error;
    std::string failed_stage;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.pending_jobs > 0) --g_state.pending_jobs;
        pending = g_state.pending_jobs;
        cancelled = g_state.cancelled_jobs;
        for (std::size_t i = 0; i < g_state.entries.size(); ++i) {
            const SegmentEntry& entry = g_state.entries[i];
            if (entry.status == SegmentStatus::Done ||
                entry.status == SegmentStatus::Placed) {
                ++done;
            } else if (entry.status == SegmentStatus::Failed) {
                ++failed;
                if (failed_index < 0) {
                    failed_index = static_cast<int>(i);
                    failed_error = entry.error;
                    failed_stage = entry.stage;
                }
            }
        }
    }
    (void)idx;
    refresh_list(parent);
    if (pending > 0) {
        set_status(parent, format_wstring(L"生成中です。残り %d 件。",
                                         pending));
        return;
    }
    // All jobs in this run are done: restore the buttons and the
    // progress display, and show the run's result.
    set_generating_ui(parent, false);
    set_progress_label(parent, L"生成状況：待機中");
    set_progress_bar(parent, 0);
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.progress_entry = -1;
        g_state.cancelled_jobs = 0;
    }
    std::wstring text;
    if (cancelled > 0) {
        text = format_wstring(L"生成を中止しました。成功 %d 件、失敗 %d 件、中止 %d 件。",
                              done, failed, cancelled);
    } else {
        text = format_wstring(L"生成が終わりました。成功 %d 件、失敗 %d 件。",
                              done, failed);
    }
    if (failed > 0 && failed_index >= 0) {
        text += format_wstring(L" 区間 %d：%s：%s", failed_index + 1,
                               stage_noun(failed_stage),
                               utf8_to_wide(failed_error).c_str());
    }
    set_status(parent, text);
    // Auto-place only when nothing was cancelled: after a cancel the
    // user may not want the finished segments placed yet.
    if (cancelled == 0 && control_checked(parent, kIdAutoPlaceCheck)) {
        std::vector<int> done_indices;
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            for (std::size_t i = 0; i < g_state.entries.size(); ++i) {
                if (g_state.entries[i].status == SegmentStatus::Done) {
                    done_indices.push_back(static_cast<int>(i));
                }
            }
        }
        if (!done_indices.empty()) {
            place_entries(parent, done_indices, /*only_done=*/true);
        }
    }
}

HWND make_control(HWND parent, const wchar_t* cls, const wchar_t* text,
                  DWORD style, int x, int y, int w, int h, int id) {
    HWND hwnd = CreateWindowExW(0, cls, text,
                                WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd != nullptr && g_font != nullptr) {
        // Without an explicit font, a control uses the system font,
        // whose size the layout cannot predict; that is what cut the
        // labels and buttons off. Every control gets the panel font.
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font),
                     TRUE);
    }
    return hwnd;
}

void build_controls(HWND hwnd) {
    // Font and DPI first: every coordinate below is a 96-DPI design
    // unit and is scaled with S(). The panel is 540 wide and 670
    // high; every control stops 12 units short of the right edge
    // (right edge at x=528). Widths are the text length at 9pt
    // (12px per full-width character) plus padding; see the header
    // comment and DESIGN.md section 18. Groups are BS_GROUPBOX
    // frames with numbered titles (design doc section 22, P1/P7);
    // the step order is also on the buttons.
    g_dpi = dpi_for_window(hwnd);
    g_font = make_ui_font(g_dpi);
    const auto S = [](int v) { return scale_px(v); };

    make_control(hwnd, L"STATIC", L"手順： 1 検出 → 2 生成 → 3 配置", 0,
                 S(12), S(8), S(396), S(16), 0);

    // 1. 検出する範囲
    make_control(hwnd, L"BUTTON", L"1. 検出する範囲", BS_GROUPBOX, S(12),
                 S(28), S(396), S(124), 0);
    make_control(hwnd, L"STATIC", L"対象レイヤー", 0, S(24), S(50), S(100),
                 S(16), 0);
    make_control(hwnd, L"EDIT", L"1", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(128), S(48), S(72), S(22), kIdLayerEdit);
    make_control(hwnd, L"STATIC", L"(1始まり)", 0, S(208), S(50), S(84),
                 S(16), 0);
    make_control(hwnd, L"STATIC", L"繋ぎ目前(f)", 0, S(24), S(76), S(100),
                 S(16), 0);
    make_control(hwnd, L"EDIT", L"30", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(128), S(74), S(64), S(22), kIdBeforeEdit);
    make_control(hwnd, L"STATIC", L"繋ぎ目後(f)", 0, S(204), S(76), S(96),
                 S(16), 0);
    make_control(hwnd, L"EDIT", L"30", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(304), S(74), S(64), S(22), kIdAfterEdit);
    // Segment mode (design doc section 26): the first radio starts
    // the group (WS_GROUP) and the next group box ends it, so the
    // two radios check each other and nothing else.
    make_control(hwnd, L"STATIC", L"区間", 0, S(24), S(102), S(100), S(16),
                 0);
    make_control(hwnd, L"BUTTON", L"カット中心",
                 BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, S(128), S(100),
                 S(120), S(20), kIdModeCutRadio);
    make_control(hwnd, L"BUTTON", L"オブジェクト単位",
                 BS_AUTORADIOBUTTON, S(252), S(100), S(148), S(20),
                 kIdModeObjectRadio);
    CheckDlgButton(hwnd,
                   load_panel_segment_mode() == SegmentMode::ObjectBased
                       ? kIdModeObjectRadio
                       : kIdModeCutRadio,
                   BST_CHECKED);
    make_control(hwnd, L"STATIC",
                 L"繋ぎ目の前後フレームを取り込みます（範囲選択時は選択範囲内）。",
                 0, S(24), S(124), S(372), S(16), 0);

    // 2. 生成する内容
    make_control(hwnd, L"BUTTON", L"2. 生成する内容",
                 BS_GROUPBOX | WS_GROUP, S(12), S(158), S(396), S(100), 0);
    make_control(hwnd, L"STATIC", L"プリセット", 0, S(24), S(180), S(100),
                 S(16), 0);
    HWND combo = make_control(hwnd, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | WS_VSCROLL, S(128), S(178),
                              S(260), S(200), kIdPresetCombo);
    for (int i = 0; i < kPresetCount; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         utf8_to_wide(preset_display_name(
                             static_cast<PresetId>(i))).c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    make_control(hwnd, L"STATIC", L"強度", 0, S(24), S(206), S(36), S(16), 0);
    make_control(hwnd, L"EDIT", L"50", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(64), S(204), S(52), S(22), kIdStrengthEdit);
    make_control(hwnd, L"STATIC", L"seed", 0, S(126), S(206), S(36), S(16), 0);
    make_control(hwnd, L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(166), S(204), S(56), S(22), kIdSeedEdit);
    make_control(hwnd, L"STATIC", L"角度(°)", 0, S(234), S(206), S(52), S(16),
                 kIdAngleLabel);
    make_control(hwnd, L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(290), S(204), S(56), S(22), kIdAngleEdit);
    make_control(hwnd, L"STATIC",
                 L"プリセットにより強度・seed・角度の有効/無効が異なります。",
                 0, S(24), S(230), S(372), S(16), 0);

    // 3. 配置と出力
    make_control(hwnd, L"BUTTON", L"3. 配置と出力", BS_GROUPBOX, S(12),
                 S(264), S(396), S(160), 0);
    make_control(hwnd, L"STATIC", L"配置先レイヤー", 0, S(24), S(286),
                 S(100), S(16), 0);
    make_control(hwnd, L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER,
                 S(128), S(284), S(52), S(22), kIdFallbackEdit);
    make_control(hwnd, L"STATIC", L"0=自動：上の空きレイヤーへ", 0, S(188),
                 S(286), S(200), S(16), 0);
    make_control(hwnd, L"BUTTON", L"MP4書き出し",
                 BS_AUTOCHECKBOX | WS_TABSTOP, S(24), S(312), S(132), S(20),
                 kIdExportCheck);
    // ffmpeg path: label on first line, edit on second line
    make_control(hwnd, L"STATIC", L"ffmpeg.exe の場所", 0, S(24), S(336),
                 S(140), S(16), 0);
    make_control(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER,
                 S(168), S(334), S(228), S(22), kIdFfmpegEdit);
    make_control(hwnd, L"STATIC",
                 L"空欄の場合は PATH から探します。",
                 0, S(24), S(360), S(372), S(16), 0);
    make_control(hwnd, L"BUTTON", L"生成後に自動で配置する",
                 BS_AUTOCHECKBOX | WS_TABSTOP, S(24), S(382), S(220), S(20),
                 kIdAutoPlaceCheck);
    CheckDlgButton(hwnd, kIdAutoPlaceCheck, BST_CHECKED);
    make_control(hwnd, L"BUTTON", L"コンソールを表示",
                 BS_AUTOCHECKBOX | WS_TABSTOP, S(248), S(382), S(156), S(20),
                 kIdConsoleCheck);
    make_control(hwnd, L"STATIC",
                 L"※ffgac / ffedit / ffmpeg の処理ウィンドウを表示",
                 0, S(24), S(406), S(372), S(16), 0);

    // Steps, in order.
    make_control(hwnd, L"BUTTON", L"1. カット点を検出", WS_TABSTOP, S(12),
                 S(436), S(192), S(28), kIdDetectButton);
    make_control(hwnd, L"BUTTON", L"2. 選択した区間を生成", WS_TABSTOP,
                 S(216), S(436), S(192), S(28), kIdGenerateButton);
    make_control(hwnd, L"BUTTON", L"生成を中止", WS_TABSTOP, S(12), S(472),
                 S(120), S(28), kIdCancelButton);
    EnableWindow(GetDlgItem(hwnd, kIdCancelButton), FALSE);
    make_control(hwnd, L"BUTTON", L"3. 選択した区間を配置する", WS_TABSTOP,
                 S(140), S(472), S(176), S(28), kIdPlaceButton);
    make_control(hwnd, L"BUTTON", L"キャッシュクリア", WS_TABSTOP, S(324),
                 S(472), S(84), S(28), kIdClearCacheButton);

    // Progress: label + bar (PROGRESS_CLASS, design doc P3).
    make_control(hwnd, L"STATIC", L"生成状況：待機中", 0, S(12), S(508),
                 S(396), S(16), kIdProgressLabel);
    HWND bar = make_control(hwnd, PROGRESS_CLASSW, L"", 0, S(12), S(528),
                            S(396), S(16), kIdProgressBar);
    SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(bar, PBM_SETPOS, 0, 0);

    // WS_HSCROLL: a long segment line scrolls instead of being cut
    // off at the list box edge. Height 72 shows about 3 entries;
    // more entries scroll.
    make_control(hwnd, L"LISTBOX", L"",
                 LBS_EXTENDEDSEL | LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL |
                     WS_BORDER,
                 S(12), S(552), S(396), S(72), kIdSegmentList);
    make_control(hwnd, L"STATIC", L"選択中: なし", 0,
                 S(12), S(628), S(396), S(18), kIdSummaryLabel);
    // No ES_AUTOHSCROLL, so long status text wraps onto the next
    // line instead of running past the right edge. Height 46 fits
    // about 2 lines at 9pt.
    make_control(hwnd, L"EDIT", L"対象レイヤーを指定して [1. カット点を検出] を押してください。",
                 ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER,
                 S(12), S(648), S(396), S(46), kIdStatusEdit);
    update_angle_control_visibility(hwnd);
}

LRESULT CALLBACK panel_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            build_controls(hwnd);
            // The creation size is in 96-DPI design units; resize to
            // the scaled size so the layout keeps its proportions at
            // 125%/150% display scaling.
            SetWindowPos(hwnd, nullptr, 0, 0, scale_px(420), scale_px(704),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (id == kIdSegmentList && code == LBN_SELCHANGE) {
                update_selection_summary(hwnd);
                return 0;
            }
            if (id == kIdPresetCombo && code == CBN_SELCHANGE) {
                update_angle_control_visibility(hwnd);
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case kIdDetectButton: on_detect(hwnd); return 0;
                    case kIdGenerateButton: on_generate(hwnd); return 0;
                    case kIdCancelButton: on_cancel(hwnd); return 0;
                    case kIdPlaceButton: on_place(hwnd); return 0;
                    case kIdClearCacheButton: on_clear_cache(hwnd); return 0;
                    case kIdConsoleCheck:
                        set_show_child_console(control_checked(hwnd, id));
                        return 0;
                    case kIdModeCutRadio:
                        save_panel_segment_mode(SegmentMode::CutCentred);
                        return 0;
                    case kIdModeObjectRadio:
                        save_panel_segment_mode(SegmentMode::ObjectBased);
                        return 0;
                    default: break;
                }
            }
            return 0;
        }
        case kMsgJobDone:
            on_job_done(hwnd, wparam);
            return 0;
        case kMsgProgress:
            on_progress(hwnd, wparam);
            return 0;
        case WM_DESTROY:
            if (g_font != nullptr && !g_font_is_stock) {
                DeleteObject(g_font);
            }
            g_font = nullptr;
            g_font_is_stock = false;
            if (g_hwnd == hwnd) g_hwnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

HWND create_panel_window(HOST_APP_TABLE* host, HINSTANCE instance) {
    if (host == nullptr || g_hwnd != nullptr) return g_hwnd;
    // The progress bar is a common control; register its class
    // before creating the panel window.
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &panel_wnd_proc;
        wc.hInstance = instance;
        // IDC_ARROW is MAKEINTRESOURCEA; the wide call needs the
        // numeric id directly (32512).
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SceneMoshPanel";
        if (RegisterClassExW(&wc) == 0) {
            log_error(&sdk_logger(), "panel: could not register window class");
            return nullptr;
        }
        class_registered = true;
    }
    HWND parent = nullptr;
    Context& ctx = context();
    if (ctx.edit != nullptr) parent = ctx.edit->get_host_app_window();
    HWND hwnd = CreateWindowExW(0, L"SceneMoshPanel", L"SceneMosh",
                                WS_CHILD | WS_VISIBLE, 0, 0, 420, 704, parent,
                                nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        log_error(&sdk_logger(), "panel: could not create the window");
        return nullptr;
    }
    host->register_window_client(L"SceneMosh", hwnd);
    g_hwnd = hwnd;
    log_info(&sdk_logger(), "SceneMosh panel created");
    return hwnd;
}

void destroy_panel_window() {
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

HWND panel_window() {
    return g_hwnd;
}

} // namespace hybrid::plugin
