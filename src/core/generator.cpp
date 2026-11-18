// generator.cpp — see generator.hpp.

#include "generator.hpp"

#include "cache_index.hpp"
#include "file_lock.hpp"
#include "hash.hpp"
#include "mp4_exporter.hpp"
#include "presets.hpp"

namespace hybrid {

Generator::Generator(FFglitchPaths tools, ILogger* logger)
    : tools_(std::move(tools)), logger_(logger) {
    worker_ = std::thread([this] { worker_loop(); });
}

Generator::~Generator() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cancel_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Generator::submit(Job job) {
    Settings settings = job.settings;
    if (!settings.normalize()) {
        JobResult result;
        result.error = "invalid settings: uid is empty or unsafe";
        if (job.finished) job.finished(result);
        return;
    }
    job.settings = settings;
    const std::vector<std::string> errors = settings.validate();
    if (!errors.empty()) {
        JobResult result;
        result.error = "invalid settings: " + errors.front();
        if (job.finished) job.finished(result);
        return;
    }
    if (!job.glitch_only && !job.provider) {
        JobResult result;
        result.error = "invalid job: frame provider is required";
        if (job.finished) job.finished(result);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(job));
    }
    cv_.notify_one();
}

void Generator::cancel() {
    std::queue<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_ = true;
        queue_.swap(dropped);
    }
    cv_.notify_all();
    // Every submitted job gets exactly one finished callback, even
    // one dropped from the queue before it started. Without this,
    // a caller counting on finished callbacks (e.g. the panel's
    // pending-job count) would wait forever for the dropped jobs.
    // The callbacks run on the cancelling thread, without the
    // generator mutex held.
    while (!dropped.empty()) {
        Job job = std::move(dropped.front());
        dropped.pop();
        if (job.finished) {
            JobResult result;
            result.cancelled = true;
            result.error = "cancelled before start";
            job.finished(result);
        }
    }
}

bool Generator::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ || !queue_.empty();
}

void Generator::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !running_ && queue_.empty(); });
}

void Generator::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            if (queue_.empty()) continue;
            job = std::move(queue_.front());
            queue_.pop();
            running_ = true;
            if (cancel_) {
                running_ = false;
                cv_.notify_all();
                if (job.finished) {
                    JobResult r;
                    r.cancelled = true;
                    r.error = "cancelled before start";
                    job.finished(r);
                }
                continue;
            }
            cancel_ = false;
        }
        JobResult result = run_job(job);
        if (job.finished) job.finished(result);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
    }
}

namespace {

bool rename_part(const std::filesystem::path& part,
                 const std::filesystem::path& final_path,
                 std::string& error) {
    std::error_code ec;
    std::filesystem::rename(part, final_path, ec);
    if (ec) {
        error = "rename failed: " + part.string() + " -> " + final_path.string() +
                ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

void Generator::ensure_tool_versions() {
    if (versions_detected_ && detected_ffgac_ == tools_.ffgac &&
        detected_ffedit_ == tools_.ffedit) {
        return;
    }
    ffgac_version_ = detect_tool_version(tools_.ffgac).value_or("unknown");
    ffedit_version_ = detect_tool_version(tools_.ffedit).value_or("unknown");
    detected_ffgac_ = tools_.ffgac;
    detected_ffedit_ = tools_.ffedit;
    versions_detected_ = true;
}

JobResult Generator::run_job(const Job& job) {
    JobResult result;
    // Local copy: the provider may report a real frame size that
    // differs from the scene size (design doc section 19), and the
    // whole job (hash, ffgac arguments, frame buffer) must use it.
    Settings settings = job.settings;
    if (job.provider) {
        int probed_w = 0;
        int probed_h = 0;
        if (job.provider->probe_frame_size(probed_w, probed_h)) {
            if (probed_w > 0 && probed_h > 0 &&
                (probed_w != settings.width ||
                 probed_h != settings.height)) {
                log_info(logger_,
                         "using rendered size " +
                             std::to_string(probed_w) + "x" +
                             std::to_string(probed_h) +
                             " instead of scene size " +
                             std::to_string(settings.width) + "x" +
                             std::to_string(settings.height));
                settings.width = probed_w;
                settings.height = probed_h;
            }
        }
        // A failed probe keeps the original size; the encode stage
        // then reports the real failure with both sizes logged.
    }
    CacheIndex cache(settings.cache_dir);
    std::error_code ec;
    std::filesystem::create_directories(settings.cache_dir, ec);

    // Tool versions are detected once per tool paths (F1) and reused
    // for every job; detection failure keeps "unknown" as before.
    ensure_tool_versions();
    const std::string ffgac_version = ffgac_version_;
    const std::string ffedit_version = ffedit_version_;
    const std::string src_hash = compute_src_hash(settings, ffgac_version);
    const std::filesystem::path src_final = cache.src_path(settings.uid, src_hash);

    auto report = [&](double p, const std::string& stage) {
        if (job.progress) job.progress(p, stage);
    };

    // Stage 1: source encode (unless glitch_only and the source exists).
    result.stage = "encode";
    const bool src_exists = std::filesystem::is_regular_file(src_final, ec);
    if (!job.glitch_only || !src_exists) {
        if (!job.provider) {
            result.error = "frame provider is required to encode the source";
            return result;
        }
        report(0.0, "encode");
        const std::filesystem::path src_part = part_path(src_final);
        EncodeResult enc = encode_source(
            *job.provider, settings, tools_, src_part, logger_,
            [&](std::int64_t done, std::int64_t total_frames) {
                report(total_frames > 0
                           ? 0.5 * static_cast<double>(done) / static_cast<double>(total_frames)
                           : 0.0,
                       "encode");
            },
            [this] { return cancel_.load(); });
        if (!enc.ok) {
            if (cancel_) {
                result.cancelled = true;
                result.error = "cancelled during encode";
            } else {
                result.error = enc.error;
            }
            return result;
        }
        if (cancel_) {
            result.cancelled = true;
            result.error = "cancelled during encode";
            return result;
        }
        if (!rename_part(src_part, src_final, result.error)) return result;
        report(0.5, "encode");
    }
    result.src_path = src_final;

    // Stage 2: glitch. Try generations 0..7 when the target file is
    // locked by playback; the generation is part of the glitch hash
    // (design doc section 9), so each attempt has its own file name.
    result.stage = "glitch";
    std::filesystem::path glitch_final;
    int generation = 0;
    for (; generation < kMaxGeneration; ++generation) {
        const std::string glitch_hash =
            compute_glitch_hash(settings, ffedit_version, generation);
        const std::filesystem::path candidate =
            cache.glitch_path(settings.uid, src_hash, glitch_hash, generation);
        if (std::filesystem::is_regular_file(candidate, ec) &&
            can_replace(candidate)) {
            // A finished file for this exact generation already exists
            // and is not locked: reuse it (seed/strength-only re-run).
            glitch_final = candidate;
            result.generation = generation;
            result.glitch_path = glitch_final;
            report(1.0, "glitch");
            result.ok = true;
            result.stage = "done";
            return result;
        }
        if (std::filesystem::is_regular_file(candidate, ec) && !can_replace(candidate)) {
            continue; // locked by playback: try the next generation
        }
        const std::filesystem::path glitch_part = part_path(candidate);
        EncodeResult app = apply_glitch(src_final, settings, tools_, glitch_part, logger_);
        if (cancel_) {
            result.cancelled = true;
            result.error = "cancelled during glitch";
            return result;
        }
        if (!app.ok) {
            result.error = app.error;
            return result;
        }
        if (!rename_part(glitch_part, candidate, result.error)) return result;
        glitch_final = candidate;
        result.generation = generation;
        break;
    }
    if (glitch_final.empty()) {
        result.error = "all generations are locked by playback";
        return result;
    }
    result.glitch_path = glitch_final;
    report(0.9, "glitch");

    // Stage 3 (optional): libx264 MP4 export.
    if (settings.export_mp4) {
        result.stage = "export";
        if (job.ffmpeg_exe.empty()) {
            result.error = "export requested but no ffmpeg path was given";
            return result;
        }
        const std::string glitch_hash =
            compute_glitch_hash(settings, ffedit_version, generation);
        const std::filesystem::path export_final =
            cache.export_path(settings.uid, src_hash, glitch_hash);
        const std::filesystem::path export_part = part_path(export_final);
        std::string export_error;
        if (!export_mp4(glitch_final, export_part, settings, job.ffmpeg_exe,
                        logger_, export_error)) {
            result.error = export_error;
            return result;
        }
        if (!rename_part(export_part, export_final, result.error)) return result;
        result.export_path = export_final;
    }

    report(1.0, "done");
    result.ok = true;
    result.stage = "done";
    return result;
}

} // namespace hybrid
