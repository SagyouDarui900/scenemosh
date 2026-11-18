// generator.hpp — one background worker that runs generation jobs.
//
// Execution model (design doc section 3): the UI thread copies its
// settings into a Job and submits it; the worker thread runs jobs one
// at a time, in submission order. Submitting while a job runs queues
// the new job instead of starting a second worker.
//
// A job is:
//   1. encode the source MP4 with ffgac (skipped when glitch_only is
//      set and a source cache file already exists),
//   2. apply the preset with ffedit,
//   3. optionally export an MP4 with ffmpeg,
//   4. rename each ".part" file to its final name.
//
// Cancellation is cooperative: the worker checks the flag between
// stages and before every frame fed to ffgac. A cancelled job leaves
// its ".part" files behind; they are never renamed. Every submitted
// job gets exactly one finished callback: a job dropped from the
// queue by cancel() is reported with cancelled=true from the
// cancelling thread.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "encoder.hpp"
#include "ffglitch.hpp"
#include "log.hpp"
#include "settings.hpp"

namespace hybrid {

struct JobResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string stage; // "encode", "glitch", "export", or "done"
    std::filesystem::path src_path;
    std::filesystem::path glitch_path;
    std::filesystem::path export_path;
    int generation = 0;
};

struct Job {
    Settings settings;
    // Reuse the existing source MP4 and only re-run ffedit.
    bool glitch_only = false;
    std::shared_ptr<IFrameProvider> provider; // required unless glitch_only
    // Optional ffmpeg for the export step (design doc section 10).
    std::filesystem::path ffmpeg_exe;
    // progress(0.0..1.0, stage) — called from the worker thread.
    std::function<void(double, const std::string&)> progress;
    // Called once, from the worker thread, when the job finishes.
    std::function<void(const JobResult&)> finished;
};

class Generator {
public:
    Generator(FFglitchPaths tools, ILogger* logger = nullptr);
    ~Generator();

    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    // Queue a job. Always succeeds (jobs run in order). The job's
    // settings are validated first; an invalid job is rejected with
    // its finished callback invoked with ok=false.
    void submit(Job job);
    // Ask the running job to stop (checked before every frame fed
    // to ffgac and between stages), and drop queued jobs. Each
    // dropped job's finished callback is invoked with cancelled=true.
    void cancel();
    [[nodiscard]] bool busy() const;
    // Block until the queue is empty and no job is running.
    void wait_idle();

private:
    void worker_loop();
    JobResult run_job(const Job& job);
    // Detect the tool versions once and reuse them (F1). run_job used
    // to spawn ffgac/ffedit once per job just to read their banners
    // (~0.22 s per job on the Linux VM). Only the worker thread
    // touches these (run_job runs there), so no extra locking is
    // needed. When the tool paths change, detect again.
    void ensure_tool_versions();

    FFglitchPaths tools_;
    ILogger* logger_;
    std::string ffgac_version_;
    std::string ffedit_version_;
    std::filesystem::path detected_ffgac_;
    std::filesystem::path detected_ffedit_;
    bool versions_detected_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Job> queue_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> cancel_{false};
    bool running_ = false; // guarded by mutex_
};

} // namespace hybrid
