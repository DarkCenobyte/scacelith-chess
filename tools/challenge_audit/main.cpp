// challenge_audit: see audit.h.
#include "audit.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

using namespace challenges;

namespace {

std::string report;   // everything printed, for --report

void say(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    report += buf;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The comment block that opens the output file (kept as it is).
std::string headerOf(const std::string& text) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') break;
        out += line + "\n";
    }
    while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0) out.pop_back();
    return out;
}

// A position's identity across sets: the set-up (without the move counters) and the lead.
std::string keyOf(const coach::ChallengePosition& p) {
    std::istringstream in(p.fen);
    std::string key, w;
    for (int i = 0; i < 4 && in >> w; ++i) key += w + " ";
    return key + "|" + p.lead;
}

struct Outcome {
    std::vector<coach::ChallengePosition> kept;
    std::vector<std::pair<const Candidate*, std::string>> rejected;
    std::vector<std::string> notes;     // per kept position, in file order: what proved it
    int checked = 0, spare = 0;
};

int usage() {
    std::fprintf(stderr,
                 "usage: challenge_audit [--candidates FILE] [--endgames FILE] [--out FILE] [--report FILE]\n"
                 "                       [--depth D] [--play-depth D] [--confirm N] [--threads T] [--hash MB]\n"
                 "                       [--only ID] [--all] [--dry-run]   (see tools/challenge_audit/audit.h)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--candidates") o.candidates = next();
        else if (a == "--endgames") o.endgames = next();
        else if (a == "--out") o.out = next();
        else if (a == "--report") o.report = next();
        else if (a == "--only") o.only = next();
        else if (a == "--depth") o.depth = std::atoi(next().c_str());
        else if (a == "--play-depth") o.playDepth = std::atoi(next().c_str());
        else if (a == "--confirm") o.confirm = std::atoi(next().c_str());
        else if (a == "--threads") o.threads = std::atoi(next().c_str());
        else if (a == "--hash") o.hashMB = std::atoi(next().c_str());
        else if (a == "--all") o.all = true;
        else if (a == "--dry-run") o.dryRun = true;
        else return usage();
    }
    if (o.depth < 1 || o.playDepth < 1 || o.confirm < 0) return usage();
    if (!o.only.empty()) o.dryRun = true;

    std::vector<Set> sets;
    std::string err;
    if (!readCandidates(o.candidates, sets, err) || (!o.endgames.empty() && !readCandidates(o.endgames, sets, err))) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::string previous;
    if (!readFile(o.out, previous) || headerOf(previous).empty()) {
        std::fprintf(stderr, "%s: cannot read the header to keep\n", o.out.c_str());
        return 2;
    }
    for (const Set& s : sets)
        if (s.want <= 0) {
            std::fprintf(stderr, "challenge %s: no '# want <n>'\n", s.id.c_str());
            return 2;
        }

    Oracle oracle(o);
    if (!oracle.ok()) {
        std::fprintf(stderr, "engine unavailable\n");
        return 2;
    }
    say("challenge_audit: depth %d, play-outs %d, confirmed %d deeper, %d thread(s), hash %d MB\n", o.depth,
        o.playDepth, o.confirm, o.threads, o.hashMB);

    const auto t0 = std::chrono::steady_clock::now();
    std::map<std::string, std::string> seen;   // keyOf -> set id
    std::vector<Outcome> outcomes(sets.size());
    for (size_t si = 0; si < sets.size(); ++si) {
        const Set& set = sets[si];
        Outcome& out = outcomes[si];
        if (!o.only.empty() && set.id != o.only) continue;
        const auto ts = std::chrono::steady_clock::now();
        for (const Candidate& c : set.candidates) {
            if (int(out.kept.size()) >= set.want && !o.all) break;
            ++out.checked;
            const auto tc = std::chrono::steady_clock::now();
            coach::ChallengePosition pos;
            std::string why, note;
            const auto it = seen.find(keyOf(c.pos));
            if (c.error.empty() && it != seen.end()) {
                why = "duplicate: also in " + it->second;
            } else {
                oracle.newCandidate();
                why = checkCandidate(oracle, set, c, pos, note);
                if (why.empty() && o.confirm > 0) {
                    // Again, deeper: the verdict must not depend on the depth.
                    coach::ChallengePosition again;
                    std::string deeper;
                    oracle.newCandidate();
                    oracle.setExtraDepth(o.confirm);
                    why = checkCandidate(oracle, set, c, again, deeper);
                    oracle.setExtraDepth(0);
                    if (!why.empty())
                        why = "unstable: " + why.substr(why.find(':') + 2) + " (" + std::to_string(o.confirm) +
                              " plies deeper; before: " + note + ")";
                    else if (again.line != pos.line || again.also != pos.also)
                        why = "unstable: " + deeper + " (" + std::to_string(o.confirm) + " plies deeper; before: " +
                              note + ")";
                    else
                        note = deeper;
                }
            }
            std::fprintf(stderr, "  %s %s %s (%.0f s): %s\n", set.id.c_str(), c.where.c_str(), c.pos.source.c_str(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count(),
                         why.empty() ? ("ok: " + note).c_str() : why.c_str());
            if (!why.empty()) {
                out.rejected.push_back({&c, why});
                continue;
            }
            if (int(out.kept.size()) >= set.want) {
                ++out.spare;
                continue;
            }
            seen[keyOf(c.pos)] = set.id;
            out.kept.push_back(pos);
            out.notes.push_back(c.pos.source + " " + std::to_string(c.pos.rating) + ": " + note);
        }
        std::stable_sort(out.kept.begin(), out.kept.end(),
                         [](const coach::ChallengePosition& a, const coach::ChallengePosition& b) {
                             return a.rating < b.rating;
                         });
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        std::fprintf(stderr, "%s: %d/%d in %.0f s\n", set.id.c_str(), int(out.kept.size()), set.want, secs);
    }

    // The report.
    bool shortSet = false;
    std::map<std::string, int> reasons;
    say("\n%-11s %5s %8s %8s %10s  %s\n", "set", "kept", "checked", "rejected", "ratings", "sources");
    for (size_t si = 0; si < sets.size(); ++si) {
        const Set& set = sets[si];
        const Outcome& out = outcomes[si];
        if (!o.only.empty() && set.id != o.only) continue;
        std::string ratings = "-";
        if (!out.kept.empty() && out.kept.back().rating > 0)
            ratings = std::to_string(out.kept.front().rating) + "-" + std::to_string(out.kept.back().rating);
        std::string sources;
        for (const coach::ChallengePosition& p : out.kept) sources += (sources.empty() ? "" : " ") + p.source;
        const bool isShort = int(out.kept.size()) < set.want;
        shortSet = shortSet || isShort;
        say("%-11s %2d/%-2d %8d %8d %10s  %s%s\n", set.id.c_str(), int(out.kept.size()), set.want, out.checked,
            int(out.rejected.size()), ratings.c_str(), sources.c_str(), isShort ? "  SHORT" : "");
        for (const auto& r : out.rejected) reasons[r.second.substr(0, r.second.find(':'))]++;
    }
    say("\nkept positions (what the deeper search proved: the score of each player move, the next best):\n");
    for (size_t si = 0; si < sets.size(); ++si)
        for (const std::string& n : outcomes[si].notes) say("  %-11s %s\n", sets[si].id.c_str(), n.c_str());
    say("\nrejections by reason:\n");
    for (const auto& r : reasons) say("  %4d  %s\n", r.second, r.first.c_str());
    say("\nrejected candidates:\n");
    for (size_t si = 0; si < sets.size(); ++si)
        for (const auto& r : outcomes[si].rejected)
            say("  %-11s %-26s %-18s %s\n", sets[si].id.c_str(), r.first->where.c_str(), r.first->pos.source.c_str(),
                r.second.c_str());
    say("\n%.0f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    // The book, read back by the game's parser.
    coach::ChallengeBook book;
    for (size_t si = 0; si < sets.size(); ++si) {
        if (outcomes[si].kept.empty()) continue;
        coach::Challenge c;
        c.id = sets[si].id;
        c.group = sets[si].group;
        c.level = sets[si].level;
        c.positions = outcomes[si].kept;
        book.challenges().push_back(c);
    }
    const std::string text = headerOf(previous) + book.write();
    std::vector<std::string> errors;
    const coach::ChallengeBook back = coach::ChallengeBook::parse(text, &errors);
    size_t positions = 0, readBack = 0;
    for (const coach::Challenge& c : book.challenges()) positions += c.positions.size();
    for (const coach::Challenge& c : back.challenges()) {
        readBack += c.positions.size();
        for (const coach::ChallengePosition& p : c.positions)
            if (c.group == "mates" && !p.endsInMate()) errors.push_back(c.id + " " + p.source + ": no checkmate");
    }
    if (readBack != positions) errors.push_back("positions read back: " + std::to_string(readBack));
    for (const std::string& e : errors) say("read back: %s\n", e.c_str());

    if (!o.dryRun && errors.empty()) {
        std::ofstream f(o.out, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
            return 2;
        }
        say("written: %s (%d challenges, %d positions)\n", o.out.c_str(), int(book.challenges().size()),
            int(positions));
    }
    if (!o.report.empty()) {
        std::ofstream f(o.report, std::ios::binary | std::ios::trunc);
        f << report;
    }
    if (!errors.empty()) return 2;
    return shortSet ? 1 : 0;
}
