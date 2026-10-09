// Files the game writes for the player: the log, screenshots, debugging dumps.
#pragma once
#include <cstdio>

namespace files {
// fopen(path, "wb") for a UTF-8 path (a wide path on Windows, whatever the process code page):
// opened for writing, created or emptied; nullptr on failure. On POSIX systems a new file gets
// mode 0644, or 0600 when privateFile, less the umask, where fopen asks for 0666: under a umask
// of 0 any user could write it. An existing private file is set to 0600 too. The descriptor is
// not inherited by the programs the game starts (O_CLOEXEC).
std::FILE* create(const char* path, bool privateFile = false);
}  // namespace files
