// Scacelith glue (not part of upstream Stockfish): the body of Stockfish 19's src/main.cpp as a
// callable function, compiled with the Stockfish sources once per instruction-set variant. The
// command line renames namespace Stockfish to Stockfish_<tag> and defines SCACELITH_SF_TAG (e.g.
// x86_64_avx2); the entry point is exported as the C function scacelith_sf_main_<tag>, one of the
// three symbols the variant's isolated object keeps global (cmake/isolate.cmake). The dispatcher
// (cpu.cpp) calls it.
#include <memory>
#include <utility>

#include "attacks.h"
#include "misc.h"
#include "position.h"
#include "tune.h"
#include "uci.h"

namespace Stockfish {
namespace {

int scacelithMain() {
    // A fake argv with argc == 1: UCIEngine::loop() then reads commands from std::cin until "quit"
    // instead of running the command-line arguments as a one-shot command. The directory the
    // engine derives from argv[0] is only searched for the network if the embedded copy failed to
    // load, which cannot happen for the default EvalFile.
    static char arg0[] = "stockfish";
    static char* argv[] = {arg0, nullptr};

    // Same sequence as upstream main(), minus the engine_info() banner. The attack and Zobrist
    // tables are filled again at every session (same values); everything else belongs to the
    // UCIEngine, so a later session (after "quit") starts from a clean state.
    Attacks::init();
    Position::init();
    CommandLine cli(1, argv);
    // On Windows CommandLine's constructor ignores its arguments and re-reads the process command
    // line (GetCommandLineW): the game's own arguments would become a one-shot UCI command.
    cli.argc = 1;
    cli.argv = argv;
    auto uci = std::make_unique<UCIEngine>(std::move(cli));
    Tune::init(uci->engine_options());
    uci->loop();  // returns on "quit" or end of input
    return 0;     // ~UCIEngine: waits for the search, joins the threads, frees the hash and the network
}

}  // namespace
}  // namespace Stockfish

#define SCACELITH_SF_CAT2(a, b) a##b
#define SCACELITH_SF_CAT(a, b) SCACELITH_SF_CAT2(a, b)

extern "C" int SCACELITH_SF_CAT(scacelith_sf_main_, SCACELITH_SF_TAG)() { return Stockfish::scacelithMain(); }
