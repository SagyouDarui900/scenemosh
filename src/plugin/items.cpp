// items.cpp — see items.hpp.

#include "items.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "cache_index.hpp"
#include "context.hpp"
#include "sdk_logger.hpp"
#include "sdk_source_reader.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace hybrid::plugin {

namespace {

// ---------------------------------------------------------------------
// Item definitions (values are the defaults for a new object).
// ---------------------------------------------------------------------

FILTER_ITEM_GROUP group_source{L"参照"};
FILTER_ITEM_SELECT::ITEM ref_mode_list[] = {
    {L"レイヤー参照", 0},
    {L"画面キャプチャ", 1},
    {nullptr, 0}};
FILTER_ITEM_GROUP group_effect{L"エフェクト"};
FILTER_ITEM_SELECT::ITEM preset_list[] = {
    {L"MV増幅", 0},
    {L"MV固定", 1},
    {L"MVランダム", 2},
    {L"データモッシュ", 3},
    {L"MV差分ドリフト", 4},
    {L"ズーム", 5},
    {L"シアー", 6},
    {L"シフト", 7},
    {L"スライス", 8},
    {L"スメア", 9},
    {L"バイブレート", 10},
    {L"ミラー", 11},
    {L"シャッフル", 12},
    {L"バッファ", 13},
    {L"ディレイ", 14},
    {L"ストップ", 15},
    {L"シンク", 16},
    {nullptr, 0}};
FILTER_ITEM_GROUP group_cache{L"キャッシュ"};
FILTER_ITEM_GROUP group_end{L""};

} // namespace

FILTER_ITEM_SELECT ref_mode_item{L"参照モード", 0, ref_mode_list};
FILTER_ITEM_TRACK ref_layer_item{L"参照レイヤー", 1, 1, 100};
FILTER_ITEM_TRACK frame_offset_item{L"フレームオフセット", 0, -600, 600};
// Lead-in / lead-out around the object's own span (design doc
// section 25): the generation reads the previous clip's frames
// before the object's start, so the motion vectors cross the cut.
// Both default to 30; 0 means no lead-in / lead-out.
FILTER_ITEM_TRACK before_frames_item{L"繋ぎ目前", 30, 0, 600};
FILTER_ITEM_TRACK after_frames_item{L"繋ぎ目後", 30, 0, 600};
FILTER_ITEM_SELECT preset_item{L"プリセット", 0, preset_list};
FILTER_ITEM_TRACK strength_item{L"強度", 50, 0, 100};
FILTER_ITEM_TRACK seed_item{L"seed", 0, 0, 100000};
FILTER_ITEM_CHECK suppress_iframe_item{L"Iフレーム抑制", true};
FILTER_ITEM_FOLDER cache_folder_item{L"キャッシュフォルダ", L""};
FILTER_ITEM_CHECK export_mp4_item{L"MP4書き出し", false};
// File path, like the panel's "ffmpeg.exe の場所" edit (design
// doc P8): the panel used to take a file path while this item took
// a folder. The file filter offers ffmpeg.exe.
FILTER_ITEM_FILE ffmpeg_file_item{
    L"ffmpeg.exe の場所", L"",
    L"ffmpeg.exe\0ffmpeg.exe\0実行ファイル (*.exe)\0*.exe\0"};
FILTER_ITEM_CHECK passthrough_item{L"素通し表示", false};
FILTER_ITEM_CHECK show_marker_item{L"未生成マークを表示", true};
FILTER_ITEM_DATA<InstanceData> instance_data_item{L"データ"};
FILTER_ITEM_BUTTON generate_button{L"生成", &on_generate_button};
FILTER_ITEM_BUTTON glitch_only_button{L"グリッチのみ再適用",
                                     &on_glitch_only_button};
FILTER_ITEM_BUTTON delete_cache_button{L"このキャッシュを削除",
                                       &on_delete_cache_button};
FILTER_ITEM_BUTTON delete_unused_button{L"未使用キャッシュを削除",
                                        &on_delete_unused_button};

void* filter_items[] = {
    &group_source,
    &ref_mode_item,
    &ref_layer_item,
    &frame_offset_item,
    &before_frames_item,
    &after_frames_item,
    &group_effect,
    &preset_item,
    &strength_item,
    &seed_item,
    &suppress_iframe_item,
    &group_cache,
    &cache_folder_item,
    &export_mp4_item,
    &ffmpeg_file_item,
    &passthrough_item,
    &show_marker_item,
    &instance_data_item,
    &generate_button,
    &glitch_only_button,
    &delete_cache_button,
    &delete_unused_button,
    &group_end,
    nullptr,
};

std::string make_uid(std::int64_t effect_id) {
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1);
    return format_string("ffglitch-%llx-%04x",
                         static_cast<unsigned long long>(effect_id),
                         n & 0xffff);
}

std::string read_uid(EDIT_SECTION* edit, OBJECT_HANDLE object,
                     EFFECT_HANDLE effect, std::int64_t effect_id) {
    if (edit != nullptr && effect != nullptr) {
        InstanceData data{};
        const int got =
            edit->get_effect_data_value(effect, L"データ", &data, sizeof(data));
        if (got == sizeof(data) && data.uid[0] != '\0') {
            return std::string(data.uid);
        }
        // No uid persisted yet. Generate one and try to persist it, so
        // the next render pass reads the same uid instead of making
        // its own (a mismatch would make the cache unfindable).
        const std::string uid = make_uid(effect_id);
        InstanceData fresh{};
        const std::size_t to_copy = std::min(uid.size(), sizeof(fresh.uid) - 1);
        std::memcpy(fresh.uid, uid.data(), to_copy);
        fresh.uid[to_copy] = '\0';
        if (!edit->set_effect_data_value(effect, L"データ", &fresh,
                                         sizeof(fresh))) {
            log_warning(&sdk_logger(),
                        "could not persist the cache uid; the cache may "
                        "be unfindable until the object has rendered once");
        }
        return uid;
    }
    (void)object;
    return make_uid(effect_id);
}

namespace {

// Clamp a lead-in / lead-out item value to its 0..600 range.
std::int64_t clamp_span_frames(std::int64_t v) {
    return std::clamp<std::int64_t>(v, 0, 600);
}

} // namespace

std::int64_t media_before_frames() {
    return clamp_span_frames(
        static_cast<std::int64_t>(before_frames_item.value));
}

std::int64_t media_after_frames() {
    return clamp_span_frames(
        static_cast<std::int64_t>(after_frames_item.value));
}

Interval media_object_interval(const OBJECT_INFO& object) {
    Interval in;
    in.start_frame =
        std::max<std::int64_t>(0, object.frame_s - media_before_frames());
    in.end_frame = object.frame_e + media_after_frames();
    return in;
}

namespace {

// Snapshot the current item values into a core Settings value.
// ref_layer is shown 1-based in the UI and converted to the SDK's
// 0-based layer number here.
Settings build_settings(const EDIT_INFO& info, const OBJECT_INFO& object,
                        std::string uid,
                        std::filesystem::path cache_dir) {
    Settings s;
    s.uid = std::move(uid);
    s.ref_mode = ref_mode_item.value == 1 ? RefMode::Screen : RefMode::Layer;
    s.ref_layer = static_cast<int>(ref_layer_item.value) - 1;
    s.frame_offset = static_cast<std::int64_t>(frame_offset_item.value);
    s.preset = static_cast<PresetId>(preset_item.value);
    s.strength = static_cast<int>(strength_item.value);
    s.seed = static_cast<std::uint32_t>(seed_item.value);
    s.suppress_iframe = suppress_iframe_item.value;
    s.export_mp4 = export_mp4_item.value;
    s.cache_dir = std::move(cache_dir);
    s.width = info.width;
    s.height = info.height;
    s.fps_num = info.rate;
    s.fps_den = info.scale;
    s.interval = media_object_interval(object);
    s.normalize();
    return s;
}

// The object whose panel the button belongs to. get_focus_object()
// returns the object selected in the settings window; fall back to
// the first selected object on the timeline.
OBJECT_HANDLE button_object(EDIT_SECTION* edit) {
    if (edit == nullptr) return nullptr;
    OBJECT_HANDLE object = edit->get_focus_object();
    if (object != nullptr) return object;
    if (edit->get_selected_object_num() > 0) {
        return edit->get_selected_object(0);
    }
    return nullptr;
}

// Where ffmpeg.exe comes from for the export step (design doc
// section 10, P8). The item holds the ffmpeg.exe file path itself,
// like the panel's edit; when empty, the bare name is used so
// Windows searches PATH. ffmpeg.exe is never bundled. When it
// cannot be found, the export step fails with an error and the
// cache MP4 is kept (see generator.cpp).
// Old projects stored a folder here (the item used to be
// "ffmpegフォルダ"); a stored folder still works by appending
// ffmpeg.exe to it.
std::filesystem::path resolve_ffmpeg_exe() {
    if (ffmpeg_file_item.value != nullptr &&
        ffmpeg_file_item.value[0] != L'\0') {
        std::filesystem::path p(ffmpeg_file_item.value);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            return p / L"ffmpeg.exe";
        }
        return p;
    }
    return std::filesystem::path(L"ffmpeg.exe");
}

void submit_generation(EDIT_SECTION* edit, bool glitch_only) {
    Context& ctx = context();
    if (ctx.edit == nullptr) {
        log_error(&sdk_logger(), "edit handle is not ready");
        return;
    }
    OBJECT_HANDLE object = button_object(edit);
    if (object == nullptr) {
        log_error(&sdk_logger(),
                  "generation: no SceneMosh object is selected");
        return;
    }
    EFFECT_HANDLE effect = edit->find_effect(object, L"SceneMosh");
    if (effect == nullptr) {
        log_error(&sdk_logger(),
                  "generation: SceneMosh effect not found on the object");
        return;
    }
    const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
    // The effect id is only known in func_proc_video; here the uid
    // seed is the object handle. The persisted uid (from the data
    // item) is preferred whenever it has been written, by a render
    // pass or by read_uid() itself.
    const std::string uid = read_uid(
        edit, object, effect,
        static_cast<std::int64_t>(
            reinterpret_cast<std::uintptr_t>(object)));

    EDIT_INFO info{};
    ctx.edit->get_edit_info(&info, sizeof(info));
    if (info.width <= 0 || info.height <= 0) {
        log_error(&sdk_logger(), "generation: scene size is unknown");
        return;
    }
    if (!confirm_ffglitch_download(nullptr)) {
        log_warning(&sdk_logger(), "generation: FFglitch download declined by user");
        return;
    }

    // Scene-relative interval of this object, for hashing and reading.
    OBJECT_INFO fake{};
    fake.frame_s = lf.start;
    fake.frame_e = lf.end;
    const std::filesystem::path cache_dir =
        resolve_cache_dir(cache_folder_item.value == nullptr
                              ? std::wstring_view{}
                              : std::wstring_view(cache_folder_item.value));
    Settings settings = build_settings(info, fake, uid, cache_dir);
    if (!settings.normalize()) {
        log_error(&sdk_logger(), "generation: invalid settings (uid)");
        return;
    }

    const int ref_layer = settings.ref_layer;
    if (settings.ref_mode == RefMode::Layer) {
        if (is_rejected_layer(ref_layer)) {
            log_error(&sdk_logger(),
                      format_string("generation: layer %d was rejected "
                                    "because it contains a framebuffer "
                                    "object",
                                    ref_layer + 1));
            return;
        }
        if (layer_has_framebuffer(edit, ref_layer,
                                  static_cast<int>(settings.interval.start_frame),
                                  static_cast<int>(settings.interval.end_frame))) {
            remember_rejected_layer(ref_layer);
            log_error(&sdk_logger(),
                      format_string("generation: layer %d contains a "
                                    "framebuffer object; generation refused",
                                    ref_layer + 1));
            return;
        }
    }

    std::shared_ptr<IFrameProvider> provider;
    if (!glitch_only) {
        const std::int64_t count = settings.interval.frame_count();
        if (settings.ref_mode == RefMode::Layer) {
            provider = std::make_shared<LayerSourceReader>(
                ctx.edit, ref_layer,
                settings.interval.start_frame + settings.frame_offset, count,
                settings.width, settings.height, &sdk_logger());
        } else {
            provider = std::make_shared<ScreenSourceReader>(
                ctx.edit,
                settings.interval.start_frame + settings.frame_offset, count,
                settings.width, settings.height, &sdk_logger());
        }
    }

    Job job;
    job.settings = settings;
    job.glitch_only = glitch_only;
    job.provider = provider;
    if (settings.export_mp4) {
        job.ffmpeg_exe = resolve_ffmpeg_exe();
    }
    job.progress = [](double progress, const std::string& stage) {
        // Log at 10% steps only; per-frame logging would flood the log.
        static thread_local double last_logged = -1.0;
        const double stepped =
            static_cast<double>(static_cast<int>(progress * 10.0)) / 10.0;
        if (stepped != last_logged) {
            last_logged = stepped;
            log_info(&sdk_logger(),
                     format_string("generating %s: %d%%", stage.c_str(),
                                   static_cast<int>(stepped * 100.0)));
        }
    };
    job.finished = [](const JobResult& result) {
        if (result.ok) {
            log_info(&sdk_logger(),
                     "generation done: " +
                         result.glitch_path.string() +
                         " (move the cursor to refresh the preview)");
        } else if (result.cancelled) {
            log_info(&sdk_logger(), "generation cancelled");
        } else {
            log_error(&sdk_logger(),
                      "generation failed at " + result.stage + ": " +
                          result.error);
        }
    };

    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        if (ctx.generator != nullptr && ctx.generator->busy()) {
            log_info(&sdk_logger(), "a job is already running; queued");
        }
    }
    ensure_tools_and_submit(std::move(job), default_tools_dir());
    log_info(&sdk_logger(), glitch_only
                                 ? "glitch-only job submitted"
                                 : "generation job submitted");
}

void delete_cache_for_uid(const std::string& uid,
                          const std::filesystem::path& cache_dir) {
    CacheIndex index(cache_dir);
    std::size_t removed = 0;
    for (const CacheEntry& entry : index.list()) {
        if (entry.uid == uid && index.remove(entry)) ++removed;
    }
    log_info(&sdk_logger(), format_string("deleted %zu cache file(s) for %s",
                                          removed, uid.c_str()));
}

} // namespace

void on_generate_button(EDIT_SECTION* edit) {
    submit_generation(edit, false);
}

void on_glitch_only_button(EDIT_SECTION* edit) {
    submit_generation(edit, true);
}

void on_delete_cache_button(EDIT_SECTION* edit) {
    OBJECT_HANDLE object = button_object(edit);
    if (object == nullptr) {
        log_error(&sdk_logger(), "delete cache: no SceneMosh object selected");
        return;
    }
    EFFECT_HANDLE effect = edit->find_effect(object, L"SceneMosh");
    const std::string uid = read_uid(
        edit, object, effect,
        static_cast<std::int64_t>(
            reinterpret_cast<std::uintptr_t>(object)));
    const std::filesystem::path cache_dir =
        resolve_cache_dir(cache_folder_item.value == nullptr
                              ? std::wstring_view{}
                              : std::wstring_view(cache_folder_item.value));
    delete_cache_for_uid(uid, cache_dir);
}

void on_delete_unused_button(EDIT_SECTION* /*edit*/) {
    // Only entries whose source file is gone are safe to delete without
    // knowing which uids are still live in other objects. ".part"
    // leftovers from interrupted jobs are deleted as well.
    const std::filesystem::path cache_dir = resolve_cache_dir(
        cache_folder_item.value == nullptr
            ? std::wstring_view{}
            : std::wstring_view(cache_folder_item.value));
    CacheIndex index(cache_dir);
    const std::vector<CacheEntry> entries = index.list();
    std::size_t removed = 0;
    for (const CacheEntry& entry : entries) {
        if (entry.is_src) continue;
        const std::filesystem::path src =
            index.src_path(entry.uid, entry.src_hash);
        std::error_code ec;
        if (!std::filesystem::exists(src, ec) && index.remove(entry)) {
            ++removed;
        }
    }
    std::error_code ec;
    for (const auto& item :
         std::filesystem::directory_iterator(cache_dir, ec)) {
        const std::filesystem::path p = item.path();
        if (p.extension() == L".mp4" && p.stem().extension() == L".part") {
            if (std::filesystem::remove(p, ec)) ++removed;
        }
    }
    log_info(&sdk_logger(),
             format_string("deleted %zu unused cache file(s)", removed));
}

} // namespace hybrid::plugin
