// Files of the repository for the tests (tests/data, protocol/), wherever the tests
// run from: under SCACELITH_SOURCE_DIR when it is set, then the working directory, its parent and
// grandparent, then the parent and grandparent of the executable's folder (build/, build-win/).
#pragma once
#include "net/net_sys.h"
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

// The folders a repository path is tried under, in that order ("" or ending with '/').
inline std::vector<std::string> repoRoots() {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("SCACELITH_SOURCE_DIR")) roots.push_back(std::string(env) + "/");
    roots.push_back("");
    roots.push_back("../");
    roots.push_back("../../");
    std::string exe = net::sys::exeDirectory();
    roots.push_back(exe + "../");
    roots.push_back(exe + "../../");
    return roots;
}

// The first copy found of a repository file of at most 'maxBytes' ("" when there is none), and
// the path it was read from in 'foundAt'.
inline std::string readRepoFile(const std::string& rel, size_t maxBytes = size_t(64) << 20, std::string* foundAt = nullptr) {
    for (auto& r : repoRoots()) {
        std::string text;
        if (net::sys::readFile(r + rel, text, maxBytes)) {
            if (foundAt) *foundAt = r + rel;
            return text;
        }
    }
    return std::string();
}
