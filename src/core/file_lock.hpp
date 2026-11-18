// file_lock.hpp — can this file be replaced right now?
//
// AviUtl2 may keep a cache MP4 open while it is playing it back. On
// Windows, opening such a file for writing fails with a sharing
// violation, which is exactly what this checks: the file is opened with
// no sharing allowed, then closed again. A missing file can always be
// "replaced" (there is nothing to replace).
//
// On POSIX there is no equivalent check — a file can be opened for
// writing even while another process reads it — so the check there is
// best-effort: it only verifies the file exists and is writable.

#pragma once

#include <filesystem>

namespace hybrid {

// True when the path does not exist, or when it exists and can be opened
// for exclusive writing.
bool can_replace(const std::filesystem::path& path) noexcept;

} // namespace hybrid
