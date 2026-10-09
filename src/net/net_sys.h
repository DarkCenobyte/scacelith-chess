// Small OS services used by the online client (internal to src/net): directories, files with
// UTF-8 paths, the system browser. The game's platform layer (src/platform, in the executable)
// returns these same directories: its exeDirectory(), userDataDirectory() and appDataDirectory()
// call the ones below.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace net {
namespace sys {

#ifdef _WIN32
std::wstring widen(const std::string& utf8);   // to UTF-16, for the W functions of Windows
// The path 'get' writes as GetModuleFileNameW does (it returns the characters written, the buffer
// size when the path was cut, 0 on failure), read again into a larger buffer while it is cut, up to
// the 32767 characters of the longest path; "" on failure. exeDirectory() passes GetModuleFileNameW.
std::wstring moduleFileName(const std::function<unsigned long(wchar_t* buffer, unsigned long size)>& get);
#endif

std::string exeDirectory();        // directory of the running executable, trailing separator
// The per-user folder of the settings (created; private, 0700, on Linux): %APPDATA%\scacelith\ on
// Windows, $XDG_CONFIG_HOME/scacelith/ (default ~/.config/scacelith/) on Linux, as a canonical path
// there.
std::string userDataDirectory();
// The per-user folder of the game's data files (created), the rule of plat::appDataDirectory():
// %APPDATA%\scacelith\ on Windows, $XDG_DATA_HOME/scacelith/ (default ~/.local/share/scacelith/)
// on Linux, as a canonical path there. The coach's voice model lives in its "coach" subfolder
// (src/tts/model_store.h).
std::string appDataDirectory();
// Where Scacelith.ini, the saved logins and the log go: userDataDirectory(), unless a Scacelith.ini
// stands next to the executable (a portable install; also where versions up to 1.0.0-beta.1 put
// it) or the user data directory cannot be written: then the executable's folder.
std::string settingsDirectory();
bool fileExists(const std::string& path);
bool directoryWritable(const std::string& dir);
bool readFile(const std::string& path, std::string& out, size_t maxBytes);
// Writes to path.tmp then replaces path. privateFile: mode 0600 on POSIX systems. On Windows a
// path open elsewhere for a moment (another thread reading it, an antivirus scan) is tried again
// for up to a second, as by renameFile().
bool writeFileAtomic(const std::string& path, const std::string& data, bool privateFile);
bool removeFile(const std::string& path);
// Opens an https:// URL in the default browser (ShellExecuteW / xdg-open). The caller checks
// the URL; this refuses anything that is not http(s) anyway.
bool openBrowser(const std::string& url);
#ifndef _WIN32
// Starts the program argv[0] (found in PATH) with argv and the game's environment, and returns its
// pid (-1: it could not be started); the caller reaps it. Its SIGPIPE has the default action: the
// game ignores that signal (main.cpp), an ignored signal stays ignored across exec, and the programs
// it starts (xdg-open's shell pipelines, a browser, a file manager) do not expect that. Every
// program the game starts goes through here (openBrowser, plat::openInFileManager).
int spawnProgram(char* const argv[]);
#endif

// ---- Files of the downloads (net/download.h, the coach's voice model): UTF-8 paths, 64-bit sizes.
std::FILE* openFile(const std::string& path, const char* mode);      // fopen / _wfopen ("rb", "wb", "ab")
bool fileSize(const std::string& path, uint64_t& size);              // false unless a regular file
bool renameFile(const std::string& from, const std::string& to);     // replaces an existing 'to'
bool directoryExists(const std::string& dir);
bool makeDirectories(const std::string& dir);                        // every missing level; true if it exists

}  // namespace sys
}  // namespace net
