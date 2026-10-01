// Small OS services used by the online client (internal to src/net): directories, files with
// UTF-8 paths, the system browser. The game's platform layer lives in the executable, not in the
// core library the network code belongs to, hence these few duplicates.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace net {
namespace sys {

std::string exeDirectory();        // directory of the running executable, trailing separator
std::string userDataDirectory();   // %APPDATA%\Scacelith\ or ~/.config/scacelith/ (created)
bool fileExists(const std::string& path);
bool directoryWritable(const std::string& dir);
bool readFile(const std::string& path, std::string& out, size_t maxBytes);
// Writes to path.tmp then replaces path. privateFile: mode 0600 on POSIX systems.
bool writeFileAtomic(const std::string& path, const std::string& data, bool privateFile);
bool removeFile(const std::string& path);
// Opens an https:// URL in the default browser (ShellExecuteW / xdg-open). The caller checks
// the URL; this refuses anything that is not http(s) anyway.
bool openBrowser(const std::string& url);

// ---- Files of the downloads (net/download.h, the coach's voice model): UTF-8 paths, 64-bit sizes.
std::FILE* openFile(const std::string& path, const char* mode);      // fopen / _wfopen ("rb", "wb", "ab")
bool fileSize(const std::string& path, uint64_t& size);              // false unless a regular file
bool renameFile(const std::string& from, const std::string& to);     // replaces an existing 'to'
bool directoryExists(const std::string& dir);
bool makeDirectories(const std::string& dir);                        // every missing level; true if it exists

}  // namespace sys
}  // namespace net
