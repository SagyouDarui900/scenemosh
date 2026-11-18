// cache_index.hpp — two-stage cache file names and directory upkeep.
//
// File names (design doc section 9):
//   <uid>-<srcHash>.src.mp4
//   <uid>-<srcHash>-<glitchHash>.mp4            (generation 0)
//   <uid>-<srcHash>-<glitchHash>-g<gen>.mp4     (generation 1..7)
//   <uid>-<srcHash>-<glitchHash>.export.mp4     (optional libx264 export)
//
// A file being written always carries ".part" before the extension
// ("<name>.part.mp4") and is renamed to its final name only when the
// writer finished successfully, so a half-written file is never mistaken
// for a finished cache entry.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hybrid {

inline constexpr int kMaxGeneration = 8; // generations 0..7

std::string src_filename(const std::string& uid, const std::string& src_hash);
std::string glitch_filename(const std::string& uid,
                            const std::string& src_hash,
                            const std::string& glitch_hash,
                            int generation);
std::string export_filename(const std::string& uid,
                            const std::string& src_hash,
                            const std::string& glitch_hash);

// Insert ".part" before the final extension: "a.mp4" -> "a.part.mp4".
std::filesystem::path part_path(const std::filesystem::path& final_path);
// Remove a trailing ".part" segment, if present.
std::filesystem::path strip_part(const std::filesystem::path& path);

// Parsed form of one cache file name. nullopt when the name is not a
// cache file at all.
struct CacheEntry {
    std::string uid;
    std::string src_hash;
    std::string glitch_hash; // empty for .src files
    int generation = 0;
    bool is_src = false;
    bool is_export = false;
    std::filesystem::path path;
};

std::optional<CacheEntry> parse_cache_filename(const std::filesystem::path& path);

class CacheIndex {
public:
    explicit CacheIndex(std::filesystem::path cache_dir) : dir_(std::move(cache_dir)) {}

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

    std::filesystem::path src_path(const std::string& uid, const std::string& src_hash) const;
    std::filesystem::path glitch_path(const std::string& uid,
                                     const std::string& src_hash,
                                     const std::string& glitch_hash,
                                     int generation) const;
    std::filesystem::path export_path(const std::string& uid,
                                      const std::string& src_hash,
                                      const std::string& glitch_hash) const;

    // All cache files currently in the directory (sorted by name).
    std::vector<CacheEntry> list() const;
    // Total size of all cache files, in bytes. 0 when the directory is missing.
    std::uint64_t total_size_bytes() const;

    // Entries whose uid is not in live_uids (or whose src file is missing
    // while a glitch file references it). Used by "delete unused cache".
    std::vector<CacheEntry> orphans(const std::vector<std::string>& live_uids) const;

    // Delete one file. Returns false when the file did not exist or
    // could not be removed.
    bool remove(const CacheEntry& entry) const;
    // Delete all orphans. Returns the number of files removed.
    std::size_t remove_orphans(const std::vector<std::string>& live_uids) const;

private:
    std::filesystem::path dir_;
};

} // namespace hybrid
