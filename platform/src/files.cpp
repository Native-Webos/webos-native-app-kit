// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/platform/files.hpp"

#include "wnk/platform/process.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace wnk::platform {

namespace {

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// /proc/mounts escapes space, tab, newline and backslash as \ooo.
std::string unescape_mount(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) && std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string extension(const std::string &name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
}

}  // namespace

std::vector<RemovableVolume> parse_removable_volumes(const std::string &mounts, bool webos) {
    static const std::set<std::string> kDesktopFs = {"vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "hfsplus", "ext4", "ext3", "ext2", "btrfs", "xfs", "udf", "iso9660"};
    std::vector<RemovableVolume> out;
    std::istringstream in(mounts);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string device, point, type, options;
        if (!(f >> device >> point >> type >> options)) continue;
        point = unescape_mount(point);
        RemovableVolume v;
        v.path = point;
        v.fs_type = type;
        v.read_only = ("," + options + ",").find(",ro,") != std::string::npos;
        if (webos) {
            // /tmp/usb/<disk>/<partition>: the partition level only (the levels above are tmpfs).
            const std::string prefix = "/tmp/usb/";
            if (point.rfind(prefix, 0) != 0 || type == "tmpfs") continue;
            const std::string rest = point.substr(prefix.size());
            const size_t slash = rest.find('/');
            if (slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos) continue;
            v.name = "USB drive " + rest.substr(slash + 1);
        } else {
            if ((point.rfind("/media/", 0) != 0 && point.rfind("/run/media/", 0) != 0) || !kDesktopFs.count(type)) continue;
            v.name = point.substr(point.find_last_of('/') + 1);
        }
        // One entry per mount point (a bind mount can list it twice).
        if (std::none_of(out.begin(), out.end(), [&](const RemovableVolume &o) { return o.path == v.path; })) out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const RemovableVolume &a, const RemovableVolume &b) { return natural_less(a.path, b.path); });
    return out;
}

std::vector<RemovableVolume> removable_volumes() {
    std::ifstream f("/proc/self/mounts");
    std::stringstream s;
    s << f.rdbuf();
    std::vector<RemovableVolume> v = parse_removable_volumes(s.str(), kIsWebOS);
    for (RemovableVolume &x : v) {
        struct statvfs st{};
        if (::statvfs(x.path.c_str(), &st) == 0) {
            x.total_bytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
            x.free_bytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
        }
    }
    return v;
}

const char *file_kind_name(FileKind k) {
    switch (k) {
        case FileKind::Folder: return "folder";
        case FileKind::Video: return "video";
        case FileKind::Audio: return "audio";
        case FileKind::Image: return "image";
        case FileKind::Subtitle: return "subtitle";
        case FileKind::Playlist: return "playlist";
        case FileKind::Other: return "other";
    }
    return "other";
}

FileKind file_kind(const std::string &name) {
    static const std::set<std::string> kVideo = {"mkv", "mp4", "m4v", "mov", "webm", "avi", "ts", "m2ts", "mts", "mpg", "mpeg", "vob", "wmv", "flv", "3gp", "ogv", "divx", "asf", "rmvb"};
    static const std::set<std::string> kAudio = {"mp3", "flac", "m4a", "aac", "ogg", "oga", "opus", "wav", "wma", "ac3", "eac3", "dts", "mka", "alac", "aiff", "aif", "ape", "wv"};
    static const std::set<std::string> kImage = {"jpg", "jpeg", "png", "gif", "webp", "bmp", "svg"};
    static const std::set<std::string> kSubtitle = {"srt", "ass", "ssa", "vtt", "ttml", "dfxp", "sub", "sup", "idx"};
    static const std::set<std::string> kPlaylist = {"m3u", "m3u8", "pls"};
    const std::string e = extension(name);
    if (kVideo.count(e)) return FileKind::Video;
    if (kAudio.count(e)) return FileKind::Audio;
    if (kImage.count(e)) return FileKind::Image;
    if (kSubtitle.count(e)) return FileKind::Subtitle;
    if (kPlaylist.count(e)) return FileKind::Playlist;
    return FileKind::Other;
}

bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            // Compare the runs of digits by value: skip leading zeros, then the longer is larger.
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return ie - is < je - js;
            const int c = a.compare(is, ie - is, b, js, je - js);
            if (c != 0) return c < 0;
            if (ie - i != je - j) return ie - i < je - j;  // "01" after "1"
            i = ie;
            j = je;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;  // equal ignoring case: a stable tie-break
}

bool list_folder(const std::string &path, std::vector<FileEntry> *out, std::string *error, size_t limit) {
    static const std::set<std::string> kSystem = {"lost.dir", "system volume information", "$recycle.bin", "recycler",
                                                  "found.000", ".trashes", ".spotlight-v100", ".fseventsd"};
    out->clear();
    DIR *d = ::opendir(path.c_str());
    if (d == nullptr) {
        if (error != nullptr) *error = std::strerror(errno);
        return false;
    }
    const std::string base = path.size() > 1 && path.back() == '/' ? path.substr(0, path.size() - 1) : path;
    while (dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || kSystem.count(lower(name))) continue;
        FileEntry f;
        f.name = name;
        f.path = base + "/" + name;
        struct stat st{};
        if (::lstat(f.path.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            f.kind = FileKind::Folder;
        } else if (S_ISREG(st.st_mode)) {
            f.kind = file_kind(name);
            f.size = static_cast<uint64_t>(st.st_size);
        } else {
            continue;
        }
        f.modified_s = static_cast<int64_t>(st.st_mtime);
        out->push_back(std::move(f));
        if (out->size() >= limit) break;
    }
    ::closedir(d);
    std::sort(out->begin(), out->end(), [](const FileEntry &a, const FileEntry &b) {
        const bool fa = a.kind == FileKind::Folder, fb = b.kind == FileKind::Folder;
        if (fa != fb) return fa;
        return natural_less(a.name, b.name);
    });
    return true;
}

std::vector<std::string> sidecar_subtitles(const std::string &video_path) {
    std::vector<std::string> out;
    const size_t slash = video_path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : video_path.substr(0, slash);
    const std::string file = slash == std::string::npos ? video_path : video_path.substr(slash + 1);
    const size_t dot = file.find_last_of('.');
    const std::string stem = lower(dot == std::string::npos || dot == 0 ? file : file.substr(0, dot));
    std::vector<FileEntry> entries;
    if (!list_folder(dir, &entries, nullptr)) return out;
    static const std::set<std::string> kText = {"srt", "ass", "ssa", "vtt", "ttml", "dfxp"};
    for (const FileEntry &e : entries) {
        if (e.kind != FileKind::Subtitle || !kText.count(extension(e.name))) continue;
        const std::string n = lower(e.name);
        // "<stem>.<ext>" or "<stem>.<anything>.<ext>"
        if (n.rfind(stem + ".", 0) != 0) continue;
        out.push_back(e.path);
    }
    return out;
}

}  // namespace wnk::platform
