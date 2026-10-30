// SPDX-License-Identifier: GPL-3.0-or-later
//
// Local media: removable drives (USB sticks and disks), folder listings that know media files, and
// the subtitle files beside a video.
//
// On the TV a drive's partitions are mounted at /tmp/usb/<disk>/<partition> (FAT, exFAT, NTFS; LG's
// own "tfat"/"texfat"/"tntfs" drivers) and bind-mounted into every app's jail at the same path, so a
// Developer Mode app reads them with plain file calls, no Luna service. removable_volumes() reads
// them from /proc/self/mounts (what this process can see); on a desktop it takes the mounts under
// /media and /run/media. Call it again to notice drives coming and going (cheap: one small file).
// Paths go to the player as "file://" + path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wnk::platform {

struct RemovableVolume {
    std::string path;     // mount point ("/tmp/usb/sda/sda1")
    std::string name;     // for people ("USB drive sda1", or the desktop's mount name)
    std::string fs_type;  // "tfat", "vfat", "exfat", ...
    bool read_only = false;
    uint64_t total_bytes = 0, free_bytes = 0;  // 0 if unknown
};

// Mounts of removable media in a /proc/mounts text (fields as the kernel escapes them: "\040" for a
// space). `webos` picks the TV's layout (/tmp/usb/...), else the desktop's (/media/..., /run/media/...).
std::vector<RemovableVolume> parse_removable_volumes(const std::string &mounts, bool webos);
// The ones this process can see now (sizes filled in), path order.
std::vector<RemovableVolume> removable_volumes();

enum class FileKind : uint8_t { Folder, Video, Audio, Image, Subtitle, Playlist, Other };
const char *file_kind_name(FileKind k);
// By extension (case-insensitive): ".mkv" → Video, ".flac" → Audio, ".srt" → Subtitle, ".m3u" → Playlist.
FileKind file_kind(const std::string &name);

struct FileEntry {
    std::string name;  // as on disk
    std::string path;  // full path
    FileKind kind = FileKind::Other;
    uint64_t size = 0;
    int64_t modified_s = 0;  // seconds since the epoch
};

// Folders first, then files, each in natural order ("Episode 2" before "Episode 10", case ignored).
// Hidden entries (".name"), the TV's and desktops' system folders ("LOST.DIR", "System Volume
// Information", "$RECYCLE.BIN") and symbolic links are left out. False with a reason if the folder
// cannot be read. At most `limit` entries.
bool list_folder(const std::string &path, std::vector<FileEntry> *out, std::string *error, size_t limit = 5000);
// Natural order of two names (true if a sorts before b).
bool natural_less(const std::string &a, const std::string &b);

// Subtitle files for a video: same folder, the video's name without its extension followed by
// ".srt", ".ass", ".ssa", ".vtt", ".ttml" or ".dfxp", or by ".<language>" and one of those
// ("Film.en.srt", "Film.forced.fr.ass"). Natural order.
std::vector<std::string> sidecar_subtitles(const std::string &video_path);

}  // namespace wnk::platform
