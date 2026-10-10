// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/log.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace wnk::log {
namespace {

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t current_thread_id() {
    // The NDK's glibc 2.12 has no gettid() wrapper; the syscall predates it everywhere.
    return static_cast<uint64_t>(syscall(SYS_gettid));
}

bool write_all(int fd, const char *data, size_t size) {
    while (size > 0) {
        ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool starts_with_ci(std::string_view text, size_t pos, std::string_view prefix) {
    return text.size() - pos >= prefix.size() && iequals(text.substr(pos, prefix.size()), prefix);
}

bool is_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

bool is_secret_key(std::string_view key) {
    static const char *const kKeys[] = {
        "token",    "access_token", "accesstoken", "refresh_token", "api_key",      "apikey",
        "password", "passwd",       "pwd",         "secret",        "client_secret", "session",
        "sessionid", "auth",        "x-plex-token", "x-emby-token", "x-mediabrowser-token",
        "authorization",
    };
    for (const char *k : kKeys) {
        if (iequals(key, k)) return true;
    }
    return false;
}

// Characters that end a secret value in free text.
bool ends_value(char c) {
    return c == '&' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' ||
           c == ',' || c == ';' || c == '}' || c == ')' || c == ']' || c == '>';
}

std::atomic<Logger *> g_default{nullptr};

}  // namespace

const char *level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        case Level::Off: return "OFF";
    }
    return "?";
}

bool parse_level(std::string_view text, Level &out) {
    static const Level kLevels[] = {Level::Trace, Level::Debug, Level::Info, Level::Warn,
                                    Level::Error, Level::Fatal, Level::Off};
    for (Level l : kLevels) {
        if (iequals(text, level_name(l))) {
            out = l;
            return true;
        }
    }
    return false;
}

std::string format_record(const Record &record) {
    char head[96];
    std::snprintf(head, sizeof head, "%10llu %-5s [", static_cast<unsigned long long>(record.monotonic_ms),
                  level_name(record.level));
    std::string line(head);
    line.append(record.category.data(), record.category.size());
    char tid[32];
    std::snprintf(tid, sizeof tid, "] (%llu) ", static_cast<unsigned long long>(record.thread_id));
    line.append(tid);
    line.append(record.message.data(), record.message.size());
    return line;
}

std::string redact(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        // "Bearer <value>" / "Basic <value>" anywhere (covers Authorization headers).
        if ((starts_with_ci(text, i, "bearer ") || starts_with_ci(text, i, "basic ")) &&
            (i == 0 || !is_key_char(text[i - 1]))) {
            const size_t word = starts_with_ci(text, i, "bearer ") ? 7 : 6;
            out.append(text.substr(i, word));
            size_t j = i + word;
            while (j < text.size() && !ends_value(text[j])) ++j;
            out.append("<redacted>");
            i = j;
            continue;
        }
        // key=value / key: value / "key":"value" where key is a credential name.
        if (is_key_char(text[i]) && (i == 0 || !is_key_char(text[i - 1]))) {
            size_t k = i;
            while (k < text.size() && is_key_char(text[k])) ++k;
            std::string_view key = text.substr(i, k - i);
            size_t j = k;
            if (j < text.size() && text[j] == '"') ++j;  // closing quote of a JSON key
            while (j < text.size() && text[j] == ' ') ++j;
            if (j < text.size() && (text[j] == '=' || text[j] == ':') && is_secret_key(key)) {
                ++j;
                while (j < text.size() && text[j] == ' ') ++j;
                bool quoted = j < text.size() && text[j] == '"';
                if (quoted) ++j;
                out.append(text.substr(i, j - i));
                size_t v = j;
                if (quoted) {
                    while (v < text.size() && text[v] != '"') ++v;
                } else if (iequals(key, "authorization")) {
                    // "Authorization: Bearer abc" -> keep the scheme word, mask the rest of the line.
                    while (v < text.size() && text[v] != '\n' && text[v] != '\r') ++v;
                } else {
                    while (v < text.size() && !ends_value(text[v])) ++v;
                }
                out.append("<redacted>");
                i = v;
                continue;
            }
            out.append(key);
            i = k;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

void StderrSink::write(const Record &, const std::string &line) {
    std::string buf = line;
    buf.push_back('\n');
    write_all(STDERR_FILENO, buf.data(), buf.size());
}

void StderrSink::flush() {}

FileSink::~FileSink() {
    if (fd_ >= 0) ::close(fd_);
}

void FileSink::write(const Record &, const std::string &line) {
    if (fd_ < 0) return;
    std::string buf = line;
    buf.push_back('\n');
    write_all(fd_, buf.data(), buf.size());  // O_APPEND: each write lands at end-of-file
}

void FileSink::flush() {
    if (fd_ >= 0) ::fdatasync(fd_);
}

int open_log_file(const std::string &path, OpenMode mode) {
    const int base = O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC;
    int fd = ::open(path.c_str(), base);
    if (fd < 0 && errno == ENOENT) {
        fd = ::open(path.c_str(), base | O_CREAT | O_EXCL, 0640);
        if (fd < 0 && errno == EEXIST) fd = ::open(path.c_str(), base);  // lost a creation race
    }
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
        ::close(fd);
        errno = EPERM;
        return -1;
    }
    if (fchmod(fd, 0640) != 0 || (mode == OpenMode::Truncate && ftruncate(fd, 0) != 0)) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

Logger::Logger(Level min_level) : min_level_(min_level), start_ns_(now_ns()) {}

void Logger::add_sink(std::unique_ptr<Sink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::set_level(Level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

Level Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_level_;
}

bool Logger::enabled(Level level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level != Level::Off && level >= min_level_;
}

void Logger::write(Level level, std::string_view category, std::string_view message) {
    if (!enabled(level)) return;
    const std::string clean = redact(message);
    Record record{level, (now_ns() - start_ns_) / 1000000ull, current_thread_id(), category, clean};
    const std::string line = format_record(record);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &sink : sinks_) sink->write(record, line);
}

void Logger::vwritef(Level level, std::string_view category, const char *fmt, va_list args) {
    if (!enabled(level)) return;
    char stack_buf[512];
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(stack_buf, sizeof stack_buf, fmt, copy);
    va_end(copy);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof stack_buf) {
        write(level, category, std::string_view(stack_buf, static_cast<size_t>(n)));
        return;
    }
    std::string heap(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(heap.data(), heap.size(), fmt, args);
    heap.resize(static_cast<size_t>(n));
    write(level, category, heap);
}

void Logger::writef(Level level, std::string_view category, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwritef(level, category, fmt, args);
    va_end(args);
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &sink : sinks_) sink->flush();
}

void set_default_logger(Logger *logger) { g_default.store(logger, std::memory_order_release); }

Logger *default_logger() { return g_default.load(std::memory_order_acquire); }

void logf(Level level, const char *category, const char *fmt, ...) {
    Logger *logger = default_logger();
    if (logger == nullptr) return;
    va_list args;
    va_start(args, fmt);
    logger->vwritef(level, category, fmt, args);
    va_end(args);
}

}  // namespace wnk::log
