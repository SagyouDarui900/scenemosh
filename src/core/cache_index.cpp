// cache_index.cpp — see cache_index.hpp.

#include "cache_index.hpp"

#include <algorithm>
#include <set>

namespace hybrid {

std::string src_filename(const std::string& uid, const std::string& src_hash) {
    return uid + "-" + src_hash + ".src.mp4";
}

std::string glitch_filename(const std::string& uid,
                            const std::string& src_hash,
                            const std::string& glitch_hash,
                            int generation) {
    std::string name = uid + "-" + src_hash + "-" + glitch_hash;
    if (generation > 0) name += "-g" + std::to_string(generation);
    return name + ".mp4";
}

std::string export_filename(const std::string& uid,
                            const std::string& src_hash,
                            const std::string& glitch_hash) {
    return uid + "-" + src_hash + "-" + glitch_hash + ".export.mp4";
}

std::filesystem::path part_path(const std::filesystem::path& final_path) {
    std::filesystem::path p = final_path;
    const std::string stem = p.stem().string();
    p.replace_filename(stem + ".part" + p.extension().string());
    return p;
}

std::filesystem::path strip_part(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    const std::string suffix = ".part.mp4";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
        name += ".mp4";
        std::filesystem::path p = path;
        p.replace_filename(name);
        return p;
    }
    return path;
}

namespace {

bool is_hex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// Split "a-b-c" on '-'. Returns false when there are not exactly the
// expected number of parts.
bool split_dashes(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find('-', start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return true;
}

} // namespace

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::optional<CacheEntry> parse_cache_filename(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    CacheEntry entry;
    entry.path = path;

    // 1. "<uid>-<srcHash>.src.mp4" — note ".src" is dot-separated.
    if (ends_with(name, ".src.mp4")) {
        const std::string base = name.substr(0, name.size() - 8); // strip ".src.mp4"
        const std::size_t last_dash = base.rfind('-');
        if (last_dash == std::string::npos || last_dash == 0) return std::nullopt;
        const std::string uid = base.substr(0, last_dash);
        const std::string src_hash = base.substr(last_dash + 1);
        if (!is_hex16(src_hash)) return std::nullopt;
        entry.uid = uid;
        entry.src_hash = src_hash;
        entry.is_src = true;
        return entry;
    }

    // 2. "<uid>-<srcHash>-<glitchHash>.export.mp4"
    if (ends_with(name, ".export.mp4")) {
        const std::string base = name.substr(0, name.size() - 11); // strip ".export.mp4"
        const std::size_t dash2 = base.rfind('-');
        if (dash2 == std::string::npos || dash2 == 0) return std::nullopt;
        const std::size_t dash1 = base.rfind('-', dash2 - 1);
        if (dash1 == std::string::npos || dash1 == 0) return std::nullopt;
        const std::string uid = base.substr(0, dash1);
        const std::string src_hash = base.substr(dash1 + 1, dash2 - dash1 - 1);
        const std::string glitch_hash = base.substr(dash2 + 1);
        if (!is_hex16(src_hash) || !is_hex16(glitch_hash)) return std::nullopt;
        entry.uid = uid;
        entry.src_hash = src_hash;
        entry.glitch_hash = glitch_hash;
        entry.is_export = true;
        return entry;
    }

    // 3. "<uid>-<srcHash>-<glitchHash>[-g<gen>].mp4"
    if (!ends_with(name, ".mp4")) return std::nullopt;
    const std::string base = name.substr(0, name.size() - 4); // strip ".mp4"

    int generation = 0;
    std::string stem = base;
    const std::size_t last_dash = stem.rfind('-');
    if (last_dash != std::string::npos && last_dash > 0) {
        const std::string part = stem.substr(last_dash + 1);
        if (part.size() >= 2 && part[0] == 'g') {
            try {
                int g = std::stoi(part.substr(1));
                if (g >= 1 && g < kMaxGeneration) {
                    generation = g;
                    stem = stem.substr(0, last_dash);
                }
            } catch (...) {}
        }
    }

    const std::size_t dash2 = stem.rfind('-');
    if (dash2 == std::string::npos || dash2 == 0) return std::nullopt;
    const std::size_t dash1 = stem.rfind('-', dash2 - 1);
    if (dash1 == std::string::npos || dash1 == 0) return std::nullopt;

    const std::string uid = stem.substr(0, dash1);
    const std::string src_hash = stem.substr(dash1 + 1, dash2 - dash1 - 1);
    const std::string glitch_hash = stem.substr(dash2 + 1);

    if (!is_hex16(src_hash) || !is_hex16(glitch_hash)) return std::nullopt;

    entry.uid = uid;
    entry.src_hash = src_hash;
    entry.glitch_hash = glitch_hash;
    entry.generation = generation;
    return entry;
}

std::filesystem::path CacheIndex::src_path(const std::string& uid,
                                           const std::string& src_hash) const {
    return dir_ / src_filename(uid, src_hash);
}

std::filesystem::path CacheIndex::glitch_path(const std::string& uid,
                                              const std::string& src_hash,
                                              const std::string& glitch_hash,
                                              int generation) const {
    return dir_ / glitch_filename(uid, src_hash, glitch_hash, generation);
}

std::filesystem::path CacheIndex::export_path(const std::string& uid,
                                              const std::string& src_hash,
                                              const std::string& glitch_hash) const {
    return dir_ / export_filename(uid, src_hash, glitch_hash);
}

std::vector<CacheEntry> CacheIndex::list() const {
    std::vector<CacheEntry> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir_, ec)) return out;
    for (const auto& item : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!item.is_regular_file(ec)) continue;
        if (auto entry = parse_cache_filename(item.path())) out.push_back(*entry);
    }
    std::sort(out.begin(), out.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.path.filename().string() < b.path.filename().string();
              });
    return out;
}

std::uint64_t CacheIndex::total_size_bytes() const {
    std::uint64_t total = 0;
    for (const CacheEntry& entry : list()) {
        std::error_code ec;
        total += std::filesystem::file_size(entry.path, ec);
    }
    return total;
}

std::vector<CacheEntry> CacheIndex::orphans(const std::vector<std::string>& live_uids) const {
    const std::set<std::string> live(live_uids.begin(), live_uids.end());
    const std::vector<CacheEntry> all = list();
    std::set<std::string> live_src_keys;
    for (const CacheEntry& e : all) {
        if (e.is_src && live.count(e.uid) != 0) {
            live_src_keys.insert(e.uid + "-" + e.src_hash);
        }
    }
    std::vector<CacheEntry> out;
    for (const CacheEntry& e : all) {
        if (live.count(e.uid) == 0) {
            out.push_back(e);
            continue;
        }
        if (!e.is_src &&
            live_src_keys.count(e.uid + "-" + e.src_hash) == 0) {
            out.push_back(e);
        }
    }
    return out;
}

bool CacheIndex::remove(const CacheEntry& entry) const {
    std::error_code ec;
    return std::filesystem::remove(entry.path, ec);
}

std::size_t CacheIndex::remove_orphans(const std::vector<std::string>& live_uids) const {
    std::size_t removed = 0;
    for (const CacheEntry& entry : orphans(live_uids)) {
        if (remove(entry)) ++removed;
    }
    return removed;
}

} // namespace hybrid
