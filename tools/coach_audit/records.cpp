// The records file of coach_audit: one block per human move.
//   rec <human w|b> <played> <startfen|startpos> ; <moves...>
//   tag <text>   (optional: where the move comes from)
//   an <slot> <ok> <wtm> <nolegal> <depth> <best|-> <lines>
//   ln <multipv> <depth> <cp> <mate> <matedNow> <matesNow> <bound E|L|U> [wdl <W> <D> <L>] <pv...>
//   hd <depth> <n> {<move> <cp> <mate> <bound>} x n   (the analysis' iterations, Analysis::history)
//   end
// "wdl": the engine's win/draw/loss per mille for the side to move (UCI_ShowWDL); the files recorded
// before it was kept have none (the reader leaves Score::hasWdl false there).
// The verification searches' cache (review --vcache) holds one block per position searched:
//   vs <fen>
//   an v ... / ln ... / hd ...   (as above)
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
        if (l.score.hasWdl) o << " wdl " << l.score.win << ' ' << l.score.draw << ' ' << l.score.loss;
        for (const std::string& m : l.pv) o << ' ' << m;
        o << '\n';
    }
    for (const ai::DepthReport& d : a.history) {
        o << "hd " << d.depth << ' ' << d.lines.size();
        for (const auto& l : d.lines)
            o << ' ' << l.first << ' ' << l.second.cp << ' ' << l.second.mate << ' ' << boundChar(l.second.bound);
        o << '\n';
    }
}

ai::Score::Bound boundOf(char b) {
    return b == 'L' ? ai::Score::Bound::Lower : b == 'U' ? ai::Score::Bound::Upper : ai::Score::Bound::Exact;
}

// The rest of an "an" line: the analysis' head.
void readHead(std::istringstream& ls, ai::Analysis& an) {
    std::string best;
    int ok = 0, wtm = 0, nl = 0, depth = 0;
    size_t n = 0;
    ls >> ok >> wtm >> nl >> depth >> best >> n;
    an = ai::Analysis();
    an.ok = ok != 0;
    an.whiteToMove = wtm != 0;
    an.noLegalMove = nl != 0;
    an.depth = depth;
    an.bestMove = best == "-" ? std::string() : best;
}

// The rest of an "ln" (a line) or "hd" (an iteration) line, added to the analysis.
void readDetail(const std::string& tag, std::istringstream& ls, ai::Analysis& an) {
    if (tag == "ln") {
        ai::PvLine l;
        int mn = 0, ms = 0;
        char b = 'E';
        ls >> l.multipv >> l.depth >> l.score.cp >> l.score.mate >> mn >> ms >> b;
        l.score.matedNow = mn != 0;
        l.score.matesNow = ms != 0;
        l.score.bound = boundOf(b);
        std::string m;
        while (ls >> m) {
            if (m == "wdl" && l.pv.empty() && !l.score.hasWdl) {
                ls >> l.score.win >> l.score.draw >> l.score.loss;
                l.score.hasWdl = true;
                continue;
            }
            l.pv.push_back(m);
        }
        an.lines.push_back(l);
        return;
    }
    ai::DepthReport d;
    size_t n = 0;
    ls >> d.depth >> n;
    for (size_t i = 0; i < n && ls; ++i) {
        std::string m;
        ai::Score sc;
        char b = 'E';
        ls >> m >> sc.cp >> sc.mate >> b;
        sc.bound = boundOf(b);
        d.lines.emplace_back(m, sc);
    }
    an.history.push_back(d);
}

}  // namespace

std::string writeSearch(const std::string& fen, const ai::Analysis& a) {
    std::ostringstream o;
    o << "vs " << fen << '\n';
    writeAnalysis(o, "v", a);
    o << "end\n";
    return o.str();
}

bool readSearches(const std::string& text, std::map<std::string, ai::Analysis>& out, std::string& err) {
    std::istringstream in(text);
    std::string line, fen;
    ai::Analysis cur;
    bool open = false;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "vs") {
            std::getline(ls, fen);
            while (!fen.empty() && fen.front() == ' ') fen.erase(fen.begin());
            cur = ai::Analysis();
            open = true;
        } else if (tag == "an" && open) {
            std::string slot;
            ls >> slot;
            readHead(ls, cur);
        } else if ((tag == "ln" || tag == "hd") && open) {
            readDetail(tag, ls, cur);
        } else if (tag == "end" && open) {
            out[fen] = cur;
            open = false;
        } else {
            err = "line " + std::to_string(lineNo) + ": unexpected " + tag;
            return false;
        }
    }
    return true;   // a block cut short (a run stopped while writing) is left out
}

std::string writeRecord(const Record& r) {
    std::ostringstream o;
    o << "rec " << (r.human == chess::White ? 'w' : 'b') << ' ' << r.played << ' '
      << (r.startFen.empty() ? std::string("startpos") : r.startFen) << " ;";
    for (const std::string& m : r.moves) o << ' ' << m;
    o << '\n';
    if (!r.tag.empty()) o << "tag " << r.tag << '\n';
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
        } else if (tag == "tag") {
            std::getline(ls, cur.tag);
            while (!cur.tag.empty() && cur.tag.front() == ' ') cur.tag.erase(cur.tag.begin());
        } else if (tag == "an") {
            std::string slot;
            ls >> slot;
            an = slot == "a0" ? &cur.a0 : slot == "a1" ? &cur.a1 : slot == "a2" ? &cur.a2 : &cur.a3;
            if (slot == "a1") cur.hasA1 = true;
            if (slot == "a2") cur.hasA2 = true;
            readHead(ls, *an);
        } else if (tag == "ln" || tag == "hd") {
            if (!an) {
                err = "line " + std::to_string(lineNo) + ": " + tag + " outside an analysis";
                return false;
            }
            readDetail(tag, ls, *an);
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
