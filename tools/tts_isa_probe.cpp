// Build check (never run): the program that third_party/stockfish/tools/isa_audit.py disassembles
// to verify that the TTS kernels compiled for AVX2, AVX-VNNI and AVX-512 (src/tts/kernels_*.cpp)
// stay confined to their objects. It links the TTS runtime from the core library the way the game
// does, next to ordinary baseline code that instantiates the standard library templates the
// runtime also uses (vectors, strings, maps, std::function, threads): if an instruction-set unit
// had emitted a shared copy of one of them, the linker could bind this code to it, and the audit
// would see a call into a kernel object from outside (see src/tts/kernels_impl.h for the rules).
#include "tts/kernels.h"
#include "tts/tts.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv, argv + argc);
    std::map<std::string, std::vector<float>> clips;
    std::function<void(const std::string&)> say = [&](const std::string& text) {
        tts::Synthesizer s;
        if (!s.load()) return;
        tts::Options o;
        o.threads = 2;
        clips[text] = s.synthesize(text, "en", o);
    };
    if (!tts::setArchCap(args.back().c_str())) return 1;
    std::thread worker([&] { say(args.back()); });
    worker.join();
    for (int level = 0; level < tts::kern::kLevelCount; ++level)
        if (const tts::kern::Table* t = tts::kern::tableFor(level)) std::printf("%s\n", t->name);
    std::printf("%s %zu\n", tts::activeArch(), clips.empty() ? size_t(0) : clips.begin()->second.size());
    return 0;
}
