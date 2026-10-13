// net::sys (net/net_sys.h), the paths tests/net_tests.cpp leaves out: openBrowser refuses every
// URL that is not http(s) before anything starts (no program is ever started here: no URL these
// tests give is one it opens), directoryWritable on a temporary folder and a missing one, and on
// Linux the user data folders without a home ($HOME unset, an XDG folder that is not absolute):
// the executable's folder.
#include "test.h"
#include "headless.h"
#include "net/net_sys.h"

#include <cstdlib>
#include <string>

TEST(net_sys_gaps_open_browser_refuses_other_schemes) {
    for (const char* url : {"", "file:///etc/hosts", "javascript:alert(1)", "ftp://example.com/", "HTTPS://example.com/",
                            " https://example.com/", "mailto:someone@example.com", "http:/example.com", "https:"})
        CHECK(!net::sys::openBrowser(url));
}

TEST(net_sys_gaps_directory_writable) {
    headless::TempDir dir("net-sys-gaps");
    CHECK(net::sys::directoryWritable(dir.path));
    CHECK(!net::sys::directoryWritable(dir.file("missing/")));
    CHECK(!net::sys::directoryWritable(dir.file("missing/deeper/")));
}

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

// Sets (or, with nullptr, unsets) an environment variable for a test's scope, restored after it.
struct ScopedEnv {
    std::string name, saved;
    bool had;
    ScopedEnv(const char* n, const char* value) : name(n) {
        const char* was = std::getenv(n);
        had = was != nullptr;
        if (had) saved = was;
        if (value) setenv(n, value, 1);
        else unsetenv(n);
    }
    ~ScopedEnv() {
        if (had) setenv(name.c_str(), saved.c_str(), 1);
        else unsetenv(name.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

}  // namespace

TEST(net_sys_gaps_data_directories_without_home) {
    // No home, and XDG folders given as relative paths (ignored): the executable's folder, and no
    // folder is made anywhere.
    const std::string exe = net::sys::exeDirectory();
    {
        ScopedEnv home("HOME", nullptr), config("XDG_CONFIG_HOME", nullptr), data("XDG_DATA_HOME", nullptr);
        CHECK_EQ(net::sys::userDataDirectory(), exe);
        CHECK_EQ(net::sys::appDataDirectory(), exe);
    }
    {
        ScopedEnv home("HOME", ""), config("XDG_CONFIG_HOME", "relative/config"), data("XDG_DATA_HOME", "relative/data");
        CHECK_EQ(net::sys::userDataDirectory(), exe);
        CHECK_EQ(net::sys::appDataDirectory(), exe);
    }
}
#endif
