// The Linux archives' res/linux/install.sh, run on a fake package in a temporary home: its libsecret
// hint follows the executable's architecture (ELF machine), with ldconfig's cache as x86-64 and
// aarch64 systems print it (a fake ldconfig first on PATH).
#if defined(__linux__)
#include "test.h"
#include "repo_files.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

bool writeFile(const std::string& path, const std::string& bytes, mode_t mode = 0644) {
    std::ofstream f(path, std::ios::binary);
    f << bytes;
    f.close();
    return f.good() && chmod(path.c_str(), mode) == 0;
}

bool run(const std::string& cmd, std::string& out) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    const int status = pclose(p);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// An ELF header (the first 20 bytes are all the script reads) of this machine.
std::string elf(unsigned machine) {
    std::string h("\x7f" "ELF\x02\x01\x01", 7);
    h.resize(16, '\0');
    h += '\x02';  // e_type: ET_EXEC
    h += '\0';
    h += char(machine & 0xFF);
    h += char(machine >> 8);
    h.resize(64, '\0');
    return h;
}

struct Package {
    std::string root, pkg, home, fake;
    bool ok = false;
    Package() {
        char tmpl[] = "/tmp/scacelith-install-XXXXXX";
        if (!mkdtemp(tmpl)) return;
        root = tmpl;
        pkg = root + "/pkg";
        home = root + "/home";
        fake = root + "/bin";
        const std::string script = readRepoFile("res/linux/install.sh");
        if (script.empty()) return;
        std::string out;
        if (!run("mkdir -p '" + pkg + "/share/applications' '" + home + "' '" + fake + "'", out)) return;
        for (int s : {16, 24, 32, 48, 64, 96, 128, 256, 512}) {
            const std::string dir = pkg + "/share/icons/hicolor/" + std::to_string(s) + "x" + std::to_string(s) + "/apps";
            if (!run("mkdir -p '" + dir + "'", out) || !writeFile(dir + "/scacelith.png", "png")) return;
        }
        ok = writeFile(pkg + "/install.sh", script, 0755) &&
             writeFile(pkg + "/share/applications/scacelith.desktop", "[Desktop Entry]\nName=Scacelith\nExec=scacelith\n") &&
             // ldconfig -p as the system prints it (the lines in $FAKE_LDCONFIG).
             writeFile(fake + "/ldconfig", "#!/bin/sh\ncat \"$FAKE_LDCONFIG\"\n", 0755);
    }
    ~Package() {
        std::string out;
        if (!root.empty()) run("rm -rf '" + root + "'", out);
    }
    // Installs an executable of this machine with this ldconfig cache: what install.sh printed.
    std::string install(unsigned machine, const std::string& cache) {
        std::string out;
        if (!writeFile(pkg + "/scacelith", elf(machine), 0755) || !writeFile(root + "/cache", cache)) return "(setup failed)";
        const std::string cmd = "env -i HOME='" + home + "' PATH='" + fake + ":/usr/bin:/bin' FAKE_LDCONFIG='" + root +
                                "/cache' sh '" + pkg + "/install.sh' 2>&1";
        if (!run(cmd, out)) return "(install.sh failed) " + out;
        return out;
    }
};

const unsigned kX86_64 = 0x3E, kAarch64 = 0xB7;
const char* kX86Line = "\tlibsecret-1.so.0 (libc6,x86-64) => /lib/x86_64-linux-gnu/libsecret-1.so.0\n";
const char* kArmLine = "\tlibsecret-1.so.0 (libc6,AArch64) => /lib/aarch64-linux-gnu/libsecret-1.so.0\n";

bool noted(const std::string& out) { return out.find("Note: libsecret-1.so.0") != std::string::npos; }

}  // namespace

TEST(install_script_libsecret_hint_follows_the_architecture) {
    Package p;
    REQUIRE(p.ok);
    const std::string header = "1234 libs found in cache `/etc/ld.so.cache'\n\tlibc.so.6 (libc6,x86-64) => /lib/libc.so.6\n";
    // x86-64 as before, and aarch64, whose cache line the old check never matched.
    std::string out = p.install(kX86_64, header + kX86Line);
    CHECK(out.find("Scacelith installed") != std::string::npos);
    CHECK(!noted(out));
    out = p.install(kAarch64, header + kArmLine);
    CHECK(out.find("Scacelith installed") != std::string::npos);
    CHECK(!noted(out));
    // A library with an ABI note in the cache.
    CHECK(!noted(p.install(kX86_64, header + "\tlibsecret-1.so.0 (libc6,x86-64, OS ABI: Linux 3.2.0) => /usr/lib64/libsecret-1.so.0\n")));
    // Only the other architecture's library: the hint, which names the architecture and says
    // what the game does when it cannot load the library either (not that it will).
#if defined(__x86_64__)
    const unsigned foreign = kAarch64;
    const char* foreignTag = "AArch64";
    const std::string hostLine = kX86Line;
    const char* foreignDir = "/usr/lib/aarch64-linux-gnu/libsecret-1.so.0";
#else
    const unsigned foreign = kX86_64;
    const char* foreignTag = "x86-64";
    const std::string hostLine = kArmLine;
    const char* foreignDir = "/usr/lib/x86_64-linux-gnu/libsecret-1.so.0";
#endif
    if (access(foreignDir, F_OK) != 0) {
        out = p.install(foreign, header + hostLine);
        CHECK(noted(out));
        CHECK(out.find(std::string("(for ") + foreignTag + ")") != std::string::npos);
        CHECK(out.find("If the game cannot load it either") != std::string::npos);
    }
    // An executable of another machine (32-bit Arm): no hint rather than a wrong one.
    CHECK(!noted(p.install(0x28, header)));
    // The launcher runs the installed executable by its full path.
    std::string desktop;
    CHECK(net::sys::readFile(p.home + "/.local/share/applications/scacelith.desktop", desktop, 4096));
    CHECK(desktop.find("Exec=\"" + p.home + "/.local/bin/scacelith\"") != std::string::npos);
}
#endif
