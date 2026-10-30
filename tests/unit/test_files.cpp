// SPDX-License-Identifier: GPL-3.0-or-later
// Local media: removable drives from mount tables (the TV's and a desktop's), media file kinds,
// natural order, folder listings and sidecar subtitles.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/platform/files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

using namespace wnk::platform;

namespace {

// From a C1 (webOS 6) with a stick in: the root's view, plus the copies in two apps' jails.
const char *kTvMounts =
    "/dev/mmcblk0p55 /mnt/lg/cache/sdp ext4 rw,nosuid,relatime,data=ordered 0 0\n"
    "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
    "tmpfs /tmp/usb tmpfs rw,relatime 0 0\n"
    "/dev/sda1 /tmp/usb/sda/sda1 tfat rw,relatime,uid=0,gid=5000,umask=0002,codepage=437 0 0\n"
    "/dev/sdb2 /tmp/usb/sdb/sdb2 texfat ro,relatime 0 0\n"
    "tmpfs /var/palm/jail/wap/tmp/usb tmpfs ro,nosuid,relatime 0 0\n"
    "/dev/sda1 /var/palm/jail/org.wnk.examples.player/tmp/usb/sda/sda1 tfat rw,relatime 0 0\n"
    "/dev/sda1 /tmp/usb/sda/sda1 tfat rw,relatime 0 0\n";  // a bind mount listed twice

const char *kDesktopMounts =
    "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n"
    "/dev/sdb1 /media/someone/My\\040Stick vfat rw,nosuid,nodev,relatime 0 0\n"
    "/dev/sdc1 /run/media/someone/BACKUP exfat rw,nosuid,nodev 0 0\n"
    "/dev/sr0 /media/someone/DVD iso9660 ro,nosuid,nodev 0 0\n"
    "tmpfs /run/user/1000 tmpfs rw 0 0\n"
    "portal /media/someone/fake fuse.portal rw 0 0\n";

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-files-XXXXXX";
    const char *d = ::mkdtemp(tmpl);
    return d != nullptr ? d : "";
}

void touch(const std::string &path, const std::string &body = "x") { std::ofstream(path) << body; }

}  // namespace

TEST("files: the TV's USB partitions, from the root's view or an app's jail") {
    const auto v = parse_removable_volumes(kTvMounts, true);
    REQUIRE(v.size() == 2);
    CHECK_EQ(v[0].path, std::string("/tmp/usb/sda/sda1"));
    CHECK_EQ(v[0].name, std::string("USB drive sda1"));
    CHECK_EQ(v[0].fs_type, std::string("tfat"));
    CHECK(!v[0].read_only);
    CHECK_EQ(v[1].path, std::string("/tmp/usb/sdb/sdb2"));
    CHECK(v[1].read_only);
    CHECK(parse_removable_volumes("", true).empty());
    CHECK(parse_removable_volumes(kDesktopMounts, true).empty());
}

TEST("files: a desktop's removable mounts, with escaped spaces") {
    const auto v = parse_removable_volumes(kDesktopMounts, false);
    REQUIRE(v.size() == 3);
    CHECK_EQ(v[0].path, std::string("/media/someone/DVD"));
    CHECK(v[0].read_only);
    CHECK_EQ(v[1].path, std::string("/media/someone/My Stick"));
    CHECK_EQ(v[1].name, std::string("My Stick"));
    CHECK_EQ(v[2].name, std::string("BACKUP"));
    CHECK(parse_removable_volumes(kTvMounts, false).empty());
}

TEST("files: kinds by extension") {
    CHECK(file_kind("Film.MKV") == FileKind::Video);
    CHECK(file_kind("clip.m2ts") == FileKind::Video);
    CHECK(file_kind("song.flac") == FileKind::Audio);
    CHECK(file_kind("cover.JPG") == FileKind::Image);
    CHECK(file_kind("Film.en.srt") == FileKind::Subtitle);
    CHECK(file_kind("list.m3u") == FileKind::Playlist);
    CHECK(file_kind("notes.txt") == FileKind::Other);
    CHECK(file_kind("mkv") == FileKind::Other);
    CHECK(file_kind(".mkv") == FileKind::Other);
    CHECK_EQ(std::string(file_kind_name(FileKind::Video)), std::string("video"));
}

TEST("files: natural order") {
    CHECK(natural_less("Episode 2", "Episode 10"));
    CHECK(!natural_less("Episode 10", "Episode 2"));
    CHECK(natural_less("a", "B"));
    CHECK(natural_less("track1", "track01"));  // equal numbers: fewer leading zeros first
    CHECK(natural_less("x9", "x10a"));
    CHECK(natural_less("abc", "abcd"));
    CHECK(!natural_less("same", "same"));
    CHECK(natural_less("S01E02", "S01E10"));
    CHECK(natural_less("99999999999999999998", "99999999999999999999"));  // longer than an integer
}

TEST("files: a folder listing: folders first, natural order, hidden and system entries left out") {
    const std::string d = temp_dir();
    REQUIRE(!d.empty());
    ::mkdir((d + "/Season 10").c_str(), 0755);
    ::mkdir((d + "/Season 2").c_str(), 0755);
    ::mkdir((d + "/LOST.DIR").c_str(), 0755);
    ::mkdir((d + "/.Trashes").c_str(), 0755);
    touch(d + "/Film 10.mkv", "0123456789");
    touch(d + "/Film 9.mkv");
    touch(d + "/.hidden.mkv");
    touch(d + "/readme.txt");
    REQUIRE(::symlink((d + "/Film 9.mkv").c_str(), (d + "/link.mkv").c_str()) == 0);
    std::vector<FileEntry> e;
    std::string err;
    REQUIRE(list_folder(d + "/", &e, &err));
    REQUIRE(e.size() == 5);
    CHECK_EQ(e[0].name, std::string("Season 2"));
    CHECK(e[0].kind == FileKind::Folder);
    CHECK_EQ(e[1].name, std::string("Season 10"));
    CHECK_EQ(e[2].name, std::string("Film 9.mkv"));
    CHECK_EQ(e[3].name, std::string("Film 10.mkv"));
    CHECK_EQ(e[3].path, d + "/Film 10.mkv");
    CHECK(e[3].kind == FileKind::Video);
    CHECK_EQ(e[3].size, uint64_t{10});
    CHECK(e[4].kind == FileKind::Other);
    CHECK(!list_folder(d + "/missing", &e, &err));
    CHECK(!err.empty());
    REQUIRE(list_folder(d, &e, &err, 2));
    CHECK_EQ(e.size(), size_t{2});
    std::system(("rm -rf '" + d + "'").c_str());
}

TEST("files: subtitles beside a video") {
    const std::string d = temp_dir();
    REQUIRE(!d.empty());
    touch(d + "/Film.mkv");
    touch(d + "/Film.srt");
    touch(d + "/film.EN.ass");
    touch(d + "/Film.forced.fr.vtt");
    touch(d + "/Film 2.srt");   // another film
    touch(d + "/Film.sup");     // a bitmap format: not loaded from a file
    touch(d + "/Filmography.srt");
    const auto s = sidecar_subtitles(d + "/Film.mkv");
    REQUIRE(s.size() == 3);
    CHECK_EQ(s[0], d + "/film.EN.ass");
    CHECK_EQ(s[1], d + "/Film.forced.fr.vtt");
    CHECK_EQ(s[2], d + "/Film.srt");
    CHECK(sidecar_subtitles(d + "/None.mkv").empty());
    std::system(("rm -rf '" + d + "'").c_str());
}
