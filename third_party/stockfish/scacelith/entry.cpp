// Scacelith glue (not part of upstream Stockfish): the body of Stockfish 16's src/main.cpp as a
// callable function, compiled with the Stockfish sources and flags.
#include "stockfish_embedded.h"

#include "bitboard.h"
#include "endgame.h"
#include "evaluate.h"
#include "misc.h"
#include "position.h"
#include "psqt.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "tt.h"
#include "tune.h"
#include "uci.h"

namespace Stockfish {
namespace {

int scacelithMain() {
    // A fake argv: no directory component, so CommandLine::init only calls getcwd() (read-only).
    // With argc == 1 UCI::loop reads commands from std::cin until "quit" instead of running a
    // one-shot command. The binary directory it derives is only consulted by Eval::NNUE::init()
    // if the embedded network failed to load, which cannot happen for the default EvalFile.
    static char arg0[] = "stockfish";
    static char* argv[] = {arg0, nullptr};
    const int argc = 1;

    // Same sequence as upstream main(), minus the engine_info() banner. Every step re-initialises
    // its globals, so a later session (after "quit") starts from a clean state.
    CommandLine::init(argc, argv);
    UCI::init(Options);
    Tune::init();
    PSQT::init();
    Bitboards::init();
    Position::init();
    Bitbases::init();
    Endgames::init();
    Threads.set(size_t(Options["Threads"]));
    Search::clear();     // After threads are up
    Eval::NNUE::init();  // EvalFile = nn-5af11540bbfe.nnue -> loaded from the "<internal>" (INCBIN) copy

    UCI::loop(argc, argv);  // returns on "quit" or end of input; never calls exit()

    Threads.set(0);
    return 0;
}

}  // namespace
}  // namespace Stockfish

int stockfish_embedded_main() { return Stockfish::scacelithMain(); }
