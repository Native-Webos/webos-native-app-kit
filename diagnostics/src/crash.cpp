// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/diagnostics/crash.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <elf.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>

namespace wnk::diagnostics {
namespace {

// Everything the handler reads lives here, filled before the handlers are installed.
char g_path[512];
char g_app[128];
char g_version[64];
char g_build_id[80];
std::atomic<const char *> g_state{"unknown"};
timespec g_started{};
volatile sig_atomic_t g_in_handler = 0;
bool g_installed = false;
alignas(16) char g_altstack[64 * 1024];
char g_maps_buf[2048];
char g_line[512];

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

void copy(char *dst, size_t cap, const std::string &src) {
    const size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// ---- async-signal-safe output ----------------------------------------------------------------------

void put(int fd, const char *s) {
    size_t n = std::strlen(s);
    while (n > 0) {
        const ssize_t w = write(fd, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        s += w;
        n -= static_cast<size_t>(w);
    }
}

void put_hex(int fd, uintptr_t v) {
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    char *p = buf + sizeof buf - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v != 0);
    *--p = 'x';
    *--p = '0';
    put(fd, p);
}

void put_dec(int fd, long long v) {
    char buf[24];
    char *p = buf + sizeof buf - 1;
    *p = '\0';
    const bool neg = v < 0;
    unsigned long long u = neg ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        *--p = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (neg) *--p = '-';
    put(fd, p);
}

const char *signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "?";
    }
}

void put_registers(int fd, void *context) {
    auto *uc = static_cast<ucontext_t *>(context);
    if (uc == nullptr) return;
#if defined(__arm__)
    put(fd, "pc=");
    put_hex(fd, uc->uc_mcontext.arm_pc);
    put(fd, " lr=");
    put_hex(fd, uc->uc_mcontext.arm_lr);
    put(fd, " sp=");
    put_hex(fd, uc->uc_mcontext.arm_sp);
    put(fd, "\n");
#elif defined(__aarch64__)
    put(fd, "pc=");
    put_hex(fd, uc->uc_mcontext.pc);
    put(fd, " lr=");
    put_hex(fd, uc->uc_mcontext.regs[30]);
    put(fd, " sp=");
    put_hex(fd, uc->uc_mcontext.sp);
    put(fd, "\n");
#elif defined(__x86_64__)
    put(fd, "pc=");
    put_hex(fd, static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
    put(fd, " sp=");
    put_hex(fd, static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]));
    put(fd, "\n");
#else
    (void)fd;
#endif
}

// Copies the executable ("r-xp") lines of /proc/self/maps.
void put_maps(int fd) {
    const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps < 0) return;
    size_t line_len = 0;
    for (;;) {
        const ssize_t n = read(maps, g_maps_buf, sizeof g_maps_buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            const char c = g_maps_buf[i];
            if (line_len < sizeof g_line - 2) g_line[line_len++] = c;
            if (c != '\n') continue;
            g_line[line_len] = '\0';
            if (std::strstr(g_line, " r-xp ") != nullptr) put(fd, g_line);
            line_len = 0;
        }
    }
    close(maps);
}

void handler(int sig, siginfo_t *info, void *context) {
    if (g_in_handler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_handler = 1;
    const int saved_errno = errno;
    const int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd >= 0) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long long uptime_ms = (now.tv_sec - g_started.tv_sec) * 1000LL + (now.tv_nsec - g_started.tv_nsec) / 1000000;
        put(fd, "wnk crash record\nsignal=");
        put_dec(fd, sig);
        put(fd, " (");
        put(fd, signal_name(sig));
        put(fd, ") code=");
        put_dec(fd, info != nullptr ? info->si_code : 0);
        put(fd, " addr=");
        put_hex(fd, reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
        put(fd, "\napp=");
        put(fd, g_app);
        put(fd, " version=");
        put(fd, g_version);
        put(fd, " build_id=");
        put(fd, g_build_id);
        put(fd, "\nstate=");
        put(fd, g_state.load(std::memory_order_relaxed));
        put(fd, " uptime_ms=");
        put_dec(fd, uptime_ms);
        put(fd, " pid=");
        put_dec(fd, getpid());
        put(fd, "\n");
        put_registers(fd, context);
        put(fd, "backtrace:\n");
        void *frames[48];
        const int n = backtrace(frames, 48);
        backtrace_symbols_fd(frames, n, fd);
        put(fd, "maps (executable):\n");
        put_maps(fd);
        put(fd, "end\n");
        close(fd);
    }
    errno = saved_errno;
    // SA_RESETHAND restored the default action. Re-raise so the process dies with the real signal;
    // for a hardware fault, returning re-executes the faulting instruction with the same effect.
    raise(sig);
}

int note_callback(dl_phdr_info *info, size_t, void *out) {
    // The first object reported is the main executable.
    auto *result = static_cast<std::string *>(out);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto *p = reinterpret_cast<const unsigned char *>(info->dlpi_addr + ph.p_vaddr);
        const auto *end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto *nh = reinterpret_cast<const ElfW(Nhdr) *>(p);
            const unsigned char *name = p + sizeof(ElfW(Nhdr));
            const unsigned char *desc = name + ((nh->n_namesz + 3) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                char hex[3];
                for (unsigned j = 0; j < nh->n_descsz; ++j) {
                    std::snprintf(hex, sizeof hex, "%02x", desc[j]);
                    *result += hex;
                }
                return 1;
            }
            p = desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
    return 1;  // only the main executable
}

}  // namespace

std::string own_build_id() {
    std::string id;
    dl_iterate_phdr(note_callback, &id);
    return id;
}

bool install_crash_handler(const CrashConfig &config, std::string *error) {
    copy(g_path, sizeof g_path, config.path);
    copy(g_app, sizeof g_app, config.app_id);
    copy(g_version, sizeof g_version, config.version);
    copy(g_build_id, sizeof g_build_id, config.build_id.empty() ? own_build_id() : config.build_id);
    if (g_installed) return true;
    clock_gettime(CLOCK_MONOTONIC, &g_started);

    // Load libgcc_s now: backtrace()'s first call does it, which is not safe inside the handler.
    void *warm[4];
    backtrace(warm, 4);

    stack_t ss{};
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof g_altstack;
    if (sigaltstack(&ss, nullptr) != 0) {
        if (error != nullptr) *error = std::string("sigaltstack: ") + std::strerror(errno);
        return false;
    }
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : kSignals) {
        if (sigaction(sig, &sa, nullptr) != 0) {
            if (error != nullptr) *error = std::string("sigaction: ") + std::strerror(errno);
            return false;
        }
    }
    g_installed = true;
    return true;
}

void set_crash_state(const char *state) { g_state.store(state, std::memory_order_relaxed); }

PreviousCrash take_previous_crash(const std::string &path) {
    PreviousCrash out;
    std::ifstream in(path);
    if (!in) return out;
    std::stringstream ss;
    ss << in.rdbuf();
    out.text = ss.str();
    out.found = !out.text.empty();
    std::istringstream lines(out.text);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("signal=", 0) == 0) {
            out.summary = line;
            break;
        }
    }
    in.close();
    std::rename(path.c_str(), (path + ".prev").c_str());
    return out;
}

}  // namespace wnk::diagnostics
