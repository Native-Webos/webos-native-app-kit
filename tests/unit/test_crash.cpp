// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash record tests: each case forks a child that installs the handler and crashes for real; the
// parent checks how the child died and what it wrote.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/diagnostics/crash.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace wnk::diagnostics;

namespace {

std::string temp_path(const char *name) {
    return std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/wnk-crash-" +
           std::to_string(getpid()) + "-" + name;
}

std::string slurp(const std::string &p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

__attribute__((noinline)) void write_to_guard_page(volatile char *p) { *p = 1; }

// Runs `crash` in a child with the handler installed; returns the signal that ended the child.
template <typename F>
int crash_child(const std::string &path, F crash) {
    std::remove(path.c_str());
    const pid_t pid = fork();
    if (pid == 0) {
        std::string err;
        if (!install_crash_handler({path, "org.wnk.test", "1.2.3", ""}, &err)) _exit(90);
        set_crash_state("Testing");
        crash();
        _exit(91);  // not reached
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : -WEXITSTATUS(status);
}

}  // namespace

TEST("crash: a real segfault writes a record and the process still dies with SIGSEGV") {
    const std::string path = temp_path("segv");
    void *page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(page != MAP_FAILED);
    const int sig = crash_child(path, [page] { write_to_guard_page(static_cast<volatile char *>(page)); });
    CHECK_EQ(sig, SIGSEGV);
    const std::string rec = slurp(path);
    CHECK(rec.find("signal=11 (SIGSEGV)") != std::string::npos);
    char addr[32];
    std::snprintf(addr, sizeof addr, "addr=%#lx", reinterpret_cast<unsigned long>(page));
    CHECK(rec.find(addr) != std::string::npos);
    CHECK(rec.find("app=org.wnk.test version=1.2.3 build_id=") != std::string::npos);
    CHECK(rec.find("state=Testing") != std::string::npos);
    CHECK(rec.find("pc=0x") != std::string::npos);
    CHECK(rec.find("backtrace:\n") != std::string::npos);
    CHECK(rec.find(" r-xp ") != std::string::npos);  // executable mappings for offline symbolisation
    CHECK(rec.find("end\n") != std::string::npos);
    munmap(page, 4096);
    std::remove(path.c_str());
}

TEST("crash: abort() is recorded as SIGABRT") {
    const std::string path = temp_path("abrt");
    const int sig = crash_child(path, [] { std::abort(); });
    CHECK_EQ(sig, SIGABRT);
    const std::string rec = slurp(path);
    CHECK(rec.find("signal=6 (SIGABRT)") != std::string::npos);
    std::remove(path.c_str());
}

TEST("crash: no record without a crash; a previous record is reported once") {
    const std::string path = temp_path("prev");
    std::remove(path.c_str());
    CHECK(!take_previous_crash(path).found);
    {
        std::ofstream out(path);
        out << "wnk crash record\nsignal=11 (SIGSEGV) code=1 addr=0x0\nend\n";
    }
    const auto prev = take_previous_crash(path);
    CHECK(prev.found);
    CHECK_EQ(prev.summary, std::string("signal=11 (SIGSEGV) code=1 addr=0x0"));
    CHECK(!take_previous_crash(path).found);  // renamed to .prev
    CHECK(!slurp(path + ".prev").empty());
    std::remove((path + ".prev").c_str());
}

TEST("crash: the main executable's build id is read when the linker wrote one") {
    const std::string id = own_build_id();
    std::printf("  build id: %s\n", id.empty() ? "<none>" : id.c_str());
    CHECK(id.empty() || id.size() >= 16);
}
