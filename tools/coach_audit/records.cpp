// The records file of coach_audit: one block per human move.
//   rec <human w|b> <played> <startfen|startpos> ; <moves...>
//   an <slot> <ok> <wtm> <nolegal> <depth> <best|-> <lines>
//   ln <multipv> <depth> <cp> <mate> <matedNow> <matesNow> <bound E|L|U> <pv...>
//   end
#include "audit.h"

#include <sstream>

namespace audit {

namespace {

char boundChar(ai::Score::Bound b) {
    return b == ai::Score::Bound::Lower ? 'L' : b == ai::Score::Bound::Upper ? 'U' : 'E';
}

void writeAnalysis(std::ostringstream& o, const char* slot, const ai::Analysis& a) {
    o << "an " << slot << ' ' << int(a.ok) << ' ' << int(a.whiteToMove) << ' ' << int(a.noLegalMove) << ' ' << a.depth
      << ' ' << (a.bestMove.empty() ? "-" : a.bestMove) << ' ' << a.lines.size() << '\n';
    for (const ai::PvLine& l : a.lines) {
        o << "ln " << l.multipv << ' ' << l.depth << ' ' << l.score.cp << ' ' << l.score.mate << ' '
          << int(l.score.matedNow) << ' ' << int(l.score.matesNow) << ' ' << boundChar(l.score.bound);
        for (const std::string& m : l.pv) o << ' ' << m;
        o << '\n';
    }
}

}  // namespace

std::string writeRecord(const Record& r) {
    std::ostringstream o;
    o << "rec " << (r.human == chess::White ? 'w' : 'b') << ' ' << r.played << ' '
      << (r.startFen.empty() ? std::string("startpos") : r.startFen) << " ;";
    for (const std::string& m : r.moves) o << ' ' << m;
    o << '\n';
    writeAnalysis(o, "a0", r.a0);
    if (r.hasA1) writeAnalysis(o, "a1", r.a1);
    if (r.hasA2) writeAnalysis(o, "a2", r.a2);
    writeAnalysis(o, "a3", r.a3);
    o << "end\n";
    return o.str();
}

bool readRecords(const std::string& text, std::vector<Record>& out, std::string& err) {
    std::istringstream in(text);
    std::string line;
    Record cur;
    ai::Analysis* an = nullptr;
    int lineNo = 0;
    bool open = false;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "rec") {
            cur = Record();
            std::string side, rest;
            ls >> side >> cur.played;
            cur.human = side == "w" ? chess::White : chess::Black;
            std::getline(ls, rest);
            const size_t semi = rest.find(" ;");
            if (semi == std::string::npos) {
                err = "line " + std::to_string(lineNo) + ": no ';'";
                return false;
            }
            std::string fen = rest.substr(0, semi);
            while (!fen.empty() && fen.front() == ' ') fen.erase(fen.begin());
            cur.startFen = fen == "startpos" ? std::string() : fen;
            std::istringstream ms(rest.substr(semi + 2));
            std::string m;
            while (ms >> m) cur.moves.push_back(m);
            open = true;
        } else if (tag == "an") {
            std::string slot, best;
            int ok = 0, wtm = 0, nl = 0, depth = 0;
            size_t n = 0;
            ls >> slot >> ok >> wtm >> nl >> depth >> best >> n;
            an = slot == "a0" ? &cur.a0 : slot == "a1" ? &cur.a1 : slot == "a2" ? &cur.a2 : &cur.a3;
            if (slot == "a1") cur.hasA1 = true;
            if (slot == "a2") cur.hasA2 = true;
            *an = ai::Analysis();
            an->ok = ok != 0;
            an->whiteToMove = wtm != 0;
            an->noLegalMove = nl != 0;
            an->depth = depth;
            an->bestMove = best == "-" ? std::string() : best;
        } else if (tag == "ln") {
            if (!an) {
                err = "line " + std::to_string(lineNo) + ": ln outside an analysis";
                return false;
            }
            ai::PvLine l;
            int mn = 0, ms = 0;
            char b = 'E';
            ls >> l.multipv >> l.depth >> l.score.cp >> l.score.mate >> mn >> ms >> b;
            l.score.matedNow = mn != 0;
            l.score.matesNow = ms != 0;
            l.score.bound = b == 'L' ? ai::Score::Bound::Lower : b == 'U' ? ai::Score::Bound::Upper : ai::Score::Bound::Exact;
            std::string m;
            while (ls >> m) l.pv.push_back(m);
            an->lines.push_back(l);
        } else if (tag == "end") {
            if (open) out.push_back(cur);
            open = false;
            an = nullptr;
        } else {
            err = "line " + std::to_string(lineNo) + ": unknown tag " + tag;
            return false;
        }
    }
    return true;
}

}  // namespace audit
