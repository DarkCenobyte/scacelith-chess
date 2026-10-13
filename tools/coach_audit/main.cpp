// coach_audit: see audit.h.
#include "audit.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    audit::Options o;
    if (argc < 2) {
        std::fprintf(stderr, "usage: coach_audit record|review|commentary [options] (see tools/coach_audit/audit.h)\n");
        return 2;
    }
    o.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--out") o.out = next();
        else if (a == "--in") o.in = next();
        else if (a == "--pgn") o.pgns.push_back(next());
        else if (a == "--selfplay") o.selfplay = std::atoi(next().c_str());
        else if (a == "--seed") o.seed = unsigned(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--depth") o.depth = std::atoi(next().c_str());
        else if (a == "--threads") o.threads = std::atoi(next().c_str());
        else if (a == "--max-plies") o.maxPlies = std::atoi(next().c_str());
        else if (a == "--level") o.level = std::atoi(next().c_str());
        else if (a == "--all") o.all = true;
        else if (a == "--sequence") o.sequence = true;
        else if (a == "--material-only") o.materialOnly = true;
        else if (a == "--hang-rate") o.hangRate = std::atof(next().c_str());
        else if (a == "--mistake-rate") o.mistakeRate = std::atof(next().c_str());
        else if (a == "--mistakes-only") o.mistakesOnly = true;
        else if (a == "--positions") o.positions.push_back(next());
        else if (a == "--band") {
            const double lo = std::atof(next().c_str());
            o.bands.push_back({lo, std::atof(next().c_str())});
        }
        else if (a == "--stop") o.stop = next();
        else if (a == "--puzzles") o.puzzles.push_back(next());
        else if (a == "--min-drop") o.minDrop = std::atof(next().c_str());
        else if (a == "--verify") o.verify = std::atoi(next().c_str());
        else if (a == "--cache") o.cache = next();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (o.mode == "record") return audit::recordMain(o);
    if (o.mode == "review") return audit::reviewMain(o);
    if (o.mode == "commentary") return audit::commentaryMain(o);
    std::fprintf(stderr, "unknown mode %s\n", o.mode.c_str());
    return 2;
}
