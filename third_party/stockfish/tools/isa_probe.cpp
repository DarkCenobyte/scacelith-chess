// Scacelith build check (not part of upstream Stockfish): the program tools/isa_audit.py
// disassembles to verify the variant isolation. It is never run. It links the library the way the
// game does, next to ordinary baseline code that instantiates the standard library templates the
// Stockfish sources also instantiate (strings, vectors, maps, string streams, threads, ...): if a
// variant's copy of one of them leaked out of its object, the linker could bind this code to it,
// and the audit would see a call into variant code from outside.
#include "stockfish_embedded.h"

#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv, argv + argc);
    std::map<std::string, int> sizes;
    for (const std::string& a : args) sizes[a] = int(a.size());
    std::istringstream in(args.back());
    std::string word;
    std::vector<int> lengths;
    while (in >> word) lengths.push_back(int(word.size()));
    auto shared = std::make_unique<std::vector<int>>(lengths);
    std::thread worker([&] { shared->resize(sizes.size() * 1000); });
    worker.join();
    std::ostringstream out;
    out << stockfish_embedded_variant_count() << ' ' << stockfish_embedded_variant(0) << ' '
        << stockfish_embedded_best_arch() << ' ' << shared->size();
    std::puts(out.str().c_str());
    if (!stockfish_embedded_limit_arch(args.back().c_str()) || !stockfish_embedded_supported()) return 1;
    std::puts(stockfish_embedded_arch());
    return stockfish_embedded_main();
}
