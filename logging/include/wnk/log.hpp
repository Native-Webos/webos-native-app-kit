// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structured, thread-safe logging for the kit.
//
// One process-wide Logger is owned by the application (no hidden singleton); the free functions
// below use the Logger installed with set_default_logger(), and do nothing if none is installed.
//
// Every message passes through redact() before it reaches a sink, so credentials embedded in URLs
// or header-like text are masked even if a caller forgets. Redaction is a safety net, not a licence
// to log secrets: callers must still avoid formatting tokens into messages.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wnk::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5, Off = 6 };

const char *level_name(Level level);
// Accepts "trace".."fatal"/"off" (case-insensitive). Returns false and leaves `out` untouched otherwise.
bool parse_level(std::string_view text, Level &out);

struct Record {
    Level level;
    uint64_t monotonic_ms;  // milliseconds since the logger was created
    uint64_t thread_id;
    std::string_view category;
    std::string_view message;  // already redacted
};

// One formatted line, without a trailing newline:
//   "<ms> <LEVEL> [<category>] (<tid>) <message>"
std::string format_record(const Record &record);

// Mask secrets: query parameters and key/value pairs whose key looks like a credential
// (token, access_token, api_key, apikey, password, passwd, secret, session, auth, x-plex-token,
// x-emby-token, authorization), "Authorization: <scheme> <value>" and "Bearer <value>".
std::string redact(std::string_view text);

class Sink {
 public:
    virtual ~Sink() = default;
    // Called with the logger's mutex held; must not call back into the logger.
    virtual void write(const Record &record, const std::string &line) = 0;
    virtual void flush() {}
};

// Writes lines to stderr.
class StderrSink final : public Sink {
 public:
    void write(const Record &record, const std::string &line) override;
    void flush() override;
};

// Appends lines to a file opened by open_log_file(). Owns the descriptor.
class FileSink final : public Sink {
 public:
    explicit FileSink(int fd) : fd_(fd) {}
    ~FileSink() override;
    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    void write(const Record &record, const std::string &line) override;
    void flush() override;

 private:
    int fd_;
};

enum class OpenMode { Truncate, Append };

// Open (or create) a log file safely in a directory other processes can write to (/tmp on the TV):
// refuses symlinks (O_NOFOLLOW), requires a regular file owned by this uid with exactly one link,
// creates new files with O_EXCL, and forces mode 0640 so the app's group (Dev Manager on an unrooted
// TV) can read it. Returns the fd, or -1 with errno set. Design follows PlxNative src/main.c.
int open_log_file(const std::string &path, OpenMode mode);

class Logger {
 public:
    explicit Logger(Level min_level = Level::Info);
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void add_sink(std::unique_ptr<Sink> sink);
    void set_level(Level level);
    Level level() const;
    bool enabled(Level level) const;

    void write(Level level, std::string_view category, std::string_view message);
    void writef(Level level, std::string_view category, const char *fmt, ...)
        __attribute__((format(printf, 4, 5)));
    void vwritef(Level level, std::string_view category, const char *fmt, va_list args);
    void flush();

 private:
    mutable std::mutex mutex_;
    Level min_level_;
    uint64_t start_ns_;
    std::vector<std::unique_ptr<Sink>> sinks_;
};

// The logger used by the free functions. The caller keeps ownership and must clear it
// (set_default_logger(nullptr)) before destroying the logger.
void set_default_logger(Logger *logger);
Logger *default_logger();

void logf(Level level, const char *category, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace wnk::log

#define WNK_LOG(level, category, ...) ::wnk::log::logf((level), (category), __VA_ARGS__)
#define WNK_LOGT(category, ...) WNK_LOG(::wnk::log::Level::Trace, category, __VA_ARGS__)
#define WNK_LOGD(category, ...) WNK_LOG(::wnk::log::Level::Debug, category, __VA_ARGS__)
#define WNK_LOGI(category, ...) WNK_LOG(::wnk::log::Level::Info, category, __VA_ARGS__)
#define WNK_LOGW(category, ...) WNK_LOG(::wnk::log::Level::Warn, category, __VA_ARGS__)
#define WNK_LOGE(category, ...) WNK_LOG(::wnk::log::Level::Error, category, __VA_ARGS__)
