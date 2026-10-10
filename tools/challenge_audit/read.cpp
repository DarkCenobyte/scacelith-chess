// challenge_audit: reading the candidates (tools/challenges/candidates.txt, endgames.txt).
#include "audit.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace challenges {

using namespace chess;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// An escape candidate: "line <src> <rating> | <fen> | - | ?" (White, the defender, to move).
Candidate readEscape(const std::string& s) {
    Candidate c;
    c.escape = true;
    std::vector<std::string> f;
    std::stringstream in(s);
    std::string x;
    while (std::getline(in, x, '|')) f.push_back(trim(x));
    const std::vector<std::string> head = f.empty() ? std::vector<std::string>() : words(f[0]);
    if (f.size() != 4 || head.size() != 3 || f[2] != "-" || f[3] != "?" || head[2].empty() ||
        head[2].find_first_not_of("0123456789") != std::string::npos || head[2].size() > 5) {
        c.error = "expected: line <source> <rating> | <fen> | - | ?";
        return c;
    }
    c.pos.source = head[1];
    c.pos.rating = std::stoi(head[2]);
    c.pos.fen = f[1];
    Position p;
    if (!p.setFEN(c.pos.fen)) c.error = "bad FEN";
    else if (p.sideToMove() != White) c.error = "the defender (White) is not to move";
    else if (!p.hasLegalMove()) c.error = "no legal move";
    return c;
}

// A line or play-out in the game's format, read by the game's own parser (legality included).
Candidate readPosition(const std::string& s) {
    Candidate c;
    std::vector<std::string> errors;
    coach::ChallengeBook b = coach::ChallengeBook::parse("challenge x mates 1\n" + s + "\n", &errors);
    if (!errors.empty() || b.challenges().empty() || b.challenges()[0].positions.size() != 1) {
        c.error = errors.empty() ? "unreadable" : errors[0].substr(errors[0].find(':') + 2);
        // Keep the source for the report.
        const std::vector<std::string> w = words(s.substr(0, s.find('|')));
        if (w.size() >= 2) c.pos.source = w[1];
        return c;
    }
    c.pos = b.challenges()[0].positions[0];
    return c;
}

}  // namespace

bool readCandidates(const std::string& path, std::vector<Set>& sets, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    std::string raw;
    int n = 0;
    Set* cur = nullptr;
    while (std::getline(in, raw)) {
        ++n;
        const std::string s = trim(raw);
        const std::vector<std::string> w = words(s.substr(0, s.find('|')));
        if (w.empty()) continue;
        if (w[0] == "#") {
            if (cur && w.size() == 3 && w[1] == "want") cur->want = std::atoi(w[2].c_str());
            continue;
        }
        if (w[0][0] == '#') continue;
        const std::string where = baseName(path) + ":" + std::to_string(n);
        if (w[0] == "challenge") {
            if (w.size() != 4) {
                err = where + ": expected: challenge <id> <group> <level>";
                return false;
            }
            cur = nullptr;
            for (Set& x : sets)
                if (x.id == w[1]) cur = &x;
            if (!cur) {
                Set x;
                x.id = w[1];
                x.group = w[2];
                x.level = std::atoi(w[3].c_str());
                sets.push_back(x);
                cur = &sets.back();
            } else if (cur->group != w[2] || cur->level != std::atoi(w[3].c_str())) {
                err = where + ": challenge " + w[1] + " has another group or level than before";
                return false;
            }
            continue;
        }
        if (!cur) {
            err = where + ": a position outside a challenge";
            return false;
        }
        const bool escape = w[0] == "line" && s.size() > 1 && s.substr(s.size() - 1) == "?";
        Candidate c = escape ? readEscape(s) : readPosition(s);
        c.where = where;
        cur->candidates.push_back(c);
    }
    return true;
}

}  // namespace challenges
