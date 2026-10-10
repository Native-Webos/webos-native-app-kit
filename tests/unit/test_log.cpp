// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/log.hpp"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wnk::log;

namespace {

struct MemorySink final : Sink {
    std::vector<std::string> *lines;
    explicit MemorySink(std::vector<std::string> *out) : lines(out) {}
    void write(const Record &, const std::string &line) override { lines->push_back(line); }
};

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-test-XXXXXX";
    const char *d = mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

std::string read_file(const std::string &path) {
    std::string out;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

}  // namespace

TEST("redact masks credential query parameters") {
    CHECK_EQ(redact("GET /items?api_key=abc123&limit=5"), std::string("GET /items?api_key=<redacted>&limit=5"));
    CHECK_EQ(redact("url=http://h/x?X-Plex-Token=SECRET"), std::string("url=http://h/x?X-Plex-Token=<redacted>"));
    CHECK_EQ(redact("access_token=zzz"), std::string("access_token=<redacted>"));
}

TEST("redact masks headers and bearer tokens") {
    CHECK_EQ(redact("Authorization: Bearer eyJabc.def"), std::string("Authorization: <redacted>"));
    CHECK_EQ(redact("sent bearer eyJabc.def ok"), std::string("sent bearer <redacted> ok"));
    CHECK_EQ(redact("password: hunter2"), std::string("password: <redacted>"));
}

TEST("redact masks JSON string values") {
    CHECK_EQ(redact(R"({"Pw":"x","password":"hunter2","user":"bob"})"),
             std::string(R"({"Pw":"x","password":"<redacted>","user":"bob"})"));
}

TEST("redact leaves ordinary text alone") {
    const std::string plain = "tokenizer=on authority=ok passwords are long; sessions: 3";
    CHECK_EQ(redact(plain), plain);
    CHECK_EQ(redact(""), std::string());
}

TEST("level names round-trip") {
    Level l = Level::Info;
    CHECK(parse_level("debug", l));
    CHECK(l == Level::Debug);
    CHECK(parse_level("WARN", l));
    CHECK(l == Level::Warn);
    CHECK(!parse_level("loud", l));
    CHECK(l == Level::Warn);
}

TEST("logger filters by level and redacts before sinks") {
    std::vector<std::string> lines;
    Logger logger(Level::Info);
    logger.add_sink(std::make_unique<MemorySink>(&lines));
    logger.write(Level::Debug, "net", "hidden");
    logger.writef(Level::Info, "net", "GET %s", "/x?token=abc");
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0].find("INFO") != std::string::npos);
    CHECK(lines[0].find("[net]") != std::string::npos);
    CHECK(lines[0].find("token=<redacted>") != std::string::npos);
    CHECK(lines[0].find("abc") == std::string::npos);
}

TEST("long formatted messages are not truncated") {
    std::vector<std::string> lines;
    Logger logger(Level::Trace);
    logger.add_sink(std::make_unique<MemorySink>(&lines));
    const std::string big(2000, 'x');
    logger.writef(Level::Info, "c", "%s|end", big.c_str());
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0].size() > 2000u);
    CHECK(lines[0].compare(lines[0].size() - 4, 4, "|end") == 0);
}

TEST("default logger can be installed and cleared") {
    std::vector<std::string> lines;
    Logger logger(Level::Trace);
    logger.add_sink(std::make_unique<MemorySink>(&lines));
    set_default_logger(&logger);
    WNK_LOGI("cat", "value=%d", 7);
    set_default_logger(nullptr);
    WNK_LOGI("cat", "dropped");
    REQUIRE(lines.size() == 1u);
    CHECK(lines[0].find("value=7") != std::string::npos);
}

TEST("open_log_file creates 0640 and truncates or appends") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    const std::string path = dir + "/a.log";
    int fd = open_log_file(path, OpenMode::Truncate);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "one\n", 4) == 4);
    ::close(fd);
    struct stat st{};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK_EQ(st.st_mode & 0777, 0640u);
    fd = open_log_file(path, OpenMode::Append);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "two\n", 4) == 4);
    ::close(fd);
    CHECK_EQ(read_file(path), std::string("one\ntwo\n"));
    fd = open_log_file(path, OpenMode::Truncate);
    REQUIRE(fd >= 0);
    ::close(fd);
    CHECK_EQ(read_file(path), std::string());
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

TEST("open_log_file refuses symlinks and hard-linked files") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    const std::string target = dir + "/target";
    const std::string link = dir + "/link.log";
    const std::string hard = dir + "/hard.log";
    int t = ::open(target.c_str(), O_WRONLY | O_CREAT, 0600);
    REQUIRE(t >= 0);
    ::close(t);
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);
    CHECK(open_log_file(link, OpenMode::Append) < 0);
    REQUIRE(::link(target.c_str(), hard.c_str()) == 0);
    CHECK(open_log_file(hard, OpenMode::Truncate) < 0);
    struct stat st{};
    REQUIRE(stat(target.c_str(), &st) == 0);
    CHECK_EQ(st.st_mode & 0777, 0600u);  // the refused open must not have chmod'ed the target
    ::unlink(link.c_str());
    ::unlink(hard.c_str());
    ::unlink(target.c_str());
    ::rmdir(dir.c_str());
}

TEST("FileSink appends formatted lines") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    const std::string path = dir + "/sink.log";
    {
        Logger logger(Level::Trace);
        int fd = open_log_file(path, OpenMode::Truncate);
        REQUIRE(fd >= 0);
        logger.add_sink(std::make_unique<FileSink>(fd));
        logger.write(Level::Warn, "io", "first");
        logger.write(Level::Error, "io", "second");
    }
    const std::string text = read_file(path);
    CHECK(text.find("WARN  [io]") != std::string::npos);
    CHECK(text.find("ERROR [io]") != std::string::npos);
    CHECK(text.find("first\n") < text.find("second\n"));
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}
