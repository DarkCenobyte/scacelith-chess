// The challenges' data: the file's parser and writer, the positions' helpers, the hints.
#include "challenge.h"
#include "../core/embedded.h"
#include "../core/log.h"
#include <algorithm>
#include <mutex>
#include <sstream>

namespace coach {
using namespace chess;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

bool parseGoal(const std::string& s, ChallengeGoal& g) {
    for (ChallengeGoal c : {ChallengeGoal::Line, ChallengeGoal::Mate, ChallengeGoal::Promote, ChallengeGoal::Hold})
        if (s == challengeGoalName(c)) {
            g = c;
            return true;
        }
    return false;
}

bool isNumber(const std::string& s) {
    return !s.empty() && s.size() < 7 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The moves of a line, played from 'p' (true when all are legal).
bool playLine(Position& p, const std::vector<std::string>& moves, size_t count) {
    for (size_t i = 0; i < count && i < moves.size(); ++i) {
        Move m = p.parseUCI(moves[i]);
        if (!m.valid()) return false;
        p.makeMove(m);
    }
    return true;
}

// What is wrong with a position's chess (empty: nothing).
std::string checkPosition(const ChallengePosition& c) {
    Position p;
    if (!p.setFEN(c.fen)) return "bad FEN";
    if (!c.lead.empty()) {
        if (p.sideToMove() != Black) return "a lead move needs Black to move";
        Move m = p.parseUCI(c.lead);
        if (!m.valid()) return "the lead move " + c.lead + " is not legal";
        p.makeMove(m);
    }
    if (p.sideToMove() != White) return "the player (White) is not to move";
    if (!p.hasLegalMove()) return "the player has no legal move";
    if (c.goal == ChallengeGoal::Line) {
        if (c.line.empty() || c.line.size() % 2 == 0) return "a line needs an odd number of moves";
        Position q = p;
        for (size_t i = 0; i < c.line.size(); ++i) {
            Move m = q.parseUCI(c.line[i]);
            if (!m.valid()) return "move " + std::to_string(i + 1) + " of the line (" + c.line[i] + ") is not legal";
            q.makeMove(m);
            if (i + 1 < c.line.size() && !q.hasLegalMove()) return "the line ends before its last move";
        }
        if (c.also.size() > size_t(c.playerMoves())) return "more alternatives than player moves";
        for (size_t k = 0; k < c.also.size(); ++k) {
            if (c.also[k].empty()) continue;
            if (k + 1 < c.also.size() || int(k) + 1 < c.playerMoves()) return "alternatives before the last move";
            Position b;
            if (!c.beforeMove(int(k), b)) return "bad line";
            for (const std::string& u : c.also[k])
                if (!b.parseUCI(u).valid()) return "the alternative " + u + " is not legal";
        }
    } else {
        if (!c.line.empty()) return "a play-out has no line";
        if (c.goal == ChallengeGoal::Hold && c.moves <= 0) return "hold needs a number of moves";
        // Nothing to play out: no side can mate any more (a goal never reached, a hold over at once).
        if (p.hasInsufficientMaterial()) return "a play-out on a dead position";
    }
    return std::string();
}

}  // namespace

const char* challengeGoalName(ChallengeGoal g) {
    switch (g) {
    case ChallengeGoal::Line: return "line";
    case ChallengeGoal::Mate: return "mate";
    case ChallengeGoal::Promote: return "promote";
    case ChallengeGoal::Hold: return "hold";
    }
    return "line";
}

// ==== Positions ===================================================================================
bool ChallengePosition::start(Position& out) const {
    Position p;
    if (!p.setFEN(fen)) return false;
    if (!lead.empty()) {
        Move m = p.parseUCI(lead);
        if (!m.valid()) return false;
        p.makeMove(m);
    }
    out = p;
    return true;
}

bool ChallengePosition::beforeMove(int k, Position& out) const {
    if (k < 0 || (goal == ChallengeGoal::Line && k >= playerMoves())) return false;
    Position p;
    if (!start(p) || !playLine(p, line, size_t(2 * k))) return false;
    out = p;
    return true;
}

bool ChallengePosition::endsInMate() const {
    if (goal != ChallengeGoal::Line || line.empty()) return false;
    Position p;
    if (!start(p) || !playLine(p, line, line.size())) return false;
    return p.isCheckmate();
}

bool ChallengePosition::accepts(int k, const Position& before, const std::string& uci) const {
    Move m = before.parseUCI(uci);
    if (!m.valid()) return false;
    const std::string u = before.toUCI(m);
    if (size_t(2 * k) < line.size() && line[size_t(2 * k)] == u) return true;
    if (size_t(k) < also.size() && std::find(also[size_t(k)].begin(), also[size_t(k)].end(), u) != also[size_t(k)].end())
        return true;
    Position after = before;
    after.makeMove(m);
    return after.isCheckmate();
}

// ==== The book ====================================================================================
ChallengeBook ChallengeBook::parse(const std::string& text, std::vector<std::string>* errors) {
    ChallengeBook book;
    auto fail = [&](int n, const std::string& msg) {
        if (errors) errors->push_back("line " + std::to_string(n) + ": " + msg);
    };
    std::istringstream in(text);
    std::string raw;
    int n = 0;
    Challenge* cur = nullptr;
    while (std::getline(in, raw)) {
        ++n;
        std::string s = trim(raw);
        if (s.empty() || s[0] == '#') continue;
        std::vector<std::string> head = words(s.substr(0, s.find('|')));
        if (head.empty()) continue;
        if (head[0] == "challenge") {
            if (head.size() != 4 || !isNumber(head[3])) {
                fail(n, "expected: challenge <id> <group> <level>");
                cur = nullptr;
                continue;
            }
            if (book.find(head[1])) {
                fail(n, "challenge " + head[1] + " defined twice");
                cur = nullptr;
                continue;
            }
            Challenge c;
            c.id = head[1];
            c.group = head[2];
            c.level = std::clamp(std::stoi(head[3]), 1, 5);
            book.challenges_.push_back(c);
            cur = &book.challenges_.back();
            continue;
        }
        if (head[0] != "line" && head[0] != "play") {
            fail(n, "unknown entry '" + head[0] + "'");
            continue;
        }
        if (!cur) {
            fail(n, "a position outside a challenge");
            continue;
        }
        std::vector<std::string> f = split(s, '|');
        for (std::string& x : f) x = trim(x);
        ChallengePosition c;
        if (head.size() < 2) {
            fail(n, "no source");
            continue;
        }
        c.source = head[1];
        if (head[0] == "line") {
            // line <source> <rating> | <fen> | <lead or -> | <moves> [| also <k>=<uci>,<uci> ...]
            if (f.size() < 4 || f.size() > 5 || head.size() != 3 || !isNumber(head[2])) {
                fail(n, "expected: line <source> <rating> | <fen> | <lead or -> | <moves> [| also ...]");
                continue;
            }
            c.rating = std::stoi(head[2]);
            c.fen = f[1];
            c.lead = f[2] == "-" ? std::string() : f[2];
            c.line = words(f[3]);
            c.goal = ChallengeGoal::Line;
            if (f.size() == 5) {
                std::vector<std::string> a = words(f[4]);
                if (a.empty() || a[0] != "also") {
                    fail(n, "the fifth field starts with 'also'");
                    continue;
                }
                c.also.assign(size_t(c.playerMoves()), {});
                bool ok = true;
                for (size_t i = 1; i < a.size(); ++i) {
                    size_t eq = a[i].find('=');
                    std::string k = eq == std::string::npos ? std::string() : a[i].substr(0, eq);
                    if (!isNumber(k) || std::stoi(k) >= c.playerMoves()) {
                        ok = false;
                        break;
                    }
                    for (const std::string& u : split(a[i].substr(eq + 1), ','))
                        if (!u.empty()) c.also[size_t(std::stoi(k))].push_back(u);
                }
                if (!ok) {
                    fail(n, "bad 'also' field");
                    continue;
                }
            }
        } else {
            // play <source> | <fen> | <goal> [<moves>]
            std::vector<std::string> g = f.size() == 3 ? words(f[2]) : std::vector<std::string>();
            if (f.size() != 3 || head.size() != 2 || g.empty() || g.size() > 2 || !parseGoal(g[0], c.goal) ||
                c.goal == ChallengeGoal::Line || (g.size() == 2 && !isNumber(g[1]))) {
                fail(n, "expected: play <source> | <fen> | mate|promote|hold [<moves>]");
                continue;
            }
            c.fen = f[1];
            if (g.size() == 2) c.moves = std::stoi(g[1]);
        }
        std::string why = checkPosition(c);
        if (!why.empty()) {
            fail(n, why);
            continue;
        }
        cur->positions.push_back(c);
    }
    // A challenge left without a position cannot be played.
    for (size_t i = 0; i < book.challenges_.size();) {
        if (book.challenges_[i].positions.empty()) {
            if (errors) errors->push_back("challenge " + book.challenges_[i].id + " has no position");
            book.challenges_.erase(book.challenges_.begin() + long(i));
        } else {
            ++i;
        }
    }
    return book;
}

std::string ChallengeBook::write() const {
    std::ostringstream o;
    for (const Challenge& c : challenges_) {
        o << "\nchallenge " << c.id << ' ' << c.group << ' ' << c.level << '\n';
        for (const ChallengePosition& p : c.positions) {
            if (p.goal == ChallengeGoal::Line) {
                o << "line " << p.source << ' ' << p.rating << " | " << p.fen << " | " << (p.lead.empty() ? "-" : p.lead) << " |";
                for (const std::string& m : p.line) o << ' ' << m;
                bool any = false;
                for (const auto& a : p.also) any = any || !a.empty();
                if (any) {
                    o << " | also";
                    for (size_t k = 0; k < p.also.size(); ++k) {
                        if (p.also[k].empty()) continue;
                        o << ' ' << k << '=';
                        for (size_t i = 0; i < p.also[k].size(); ++i) o << (i ? "," : "") << p.also[k][i];
                    }
                }
                o << '\n';
            } else {
                o << "play " << p.source << " | " << p.fen << " | " << challengeGoalName(p.goal);
                if (p.moves > 0) o << ' ' << p.moves;
                o << '\n';
            }
        }
    }
    return o.str();
}

const ChallengeBook& ChallengeBook::shared() {
    static ChallengeBook book;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::string> errors;
        book = parse(embedded::text("assets/coach/challenges/challenges.txt"), &errors);
        for (const std::string& e : errors) LOGW("coach challenges: %s", e.c_str());
        size_t positions = 0;
        for (const Challenge& c : book.challenges()) positions += c.positions.size();
        LOGI("coach challenges: %d challenges, %d positions", int(book.challenges().size()), int(positions));
    });
    return book;
}

const Challenge* ChallengeBook::find(const std::string& id) const {
    for (const Challenge& c : challenges_)
        if (c.id == id) return &c;
    return nullptr;
}

int ChallengeBook::indexOf(const std::string& id) const {
    for (size_t i = 0; i < challenges_.size(); ++i)
        if (challenges_[i].id == id) return int(i);
    return -1;
}

std::vector<std::string> ChallengeBook::groups() const {
    std::vector<std::string> out;
    for (const Challenge& c : challenges_)
        if (std::find(out.begin(), out.end(), c.group) == out.end()) out.push_back(c.group);
    return out;
}

// ==== Hints =======================================================================================
namespace {
// Only the piece on 'from' has a legal move: naming it would give nothing away.
bool onlyPieceToMove(const Position& pos, Square from) {
    for (const Move& m : pos.legalMoves())
        if (m.from != from) return false;
    return true;
}
}  // namespace

int challengeHintSteps(const Position& pos, const std::string& uci) {
    Move m = pos.parseUCI(uci);
    if (!m.valid()) return 0;
    return onlyPieceToMove(pos, m.from) ? 2 : 3;
}

ChallengeHint challengeHint(const Position& pos, const std::string& uci, int step) {
    ChallengeHint h;
    Move m = pos.parseUCI(uci);
    if (!m.valid() || step < 1) return h;
    h.piece = m.from;
    h.square = m.to;
    const bool only = onlyPieceToMove(pos, m.from);
    if (!only && step == 1) h.kind = ChallengeHint::Kind::Piece;
    else if ((only && step == 1) || (!only && step == 2)) h.kind = ChallengeHint::Kind::Square;
    else h.kind = ChallengeHint::Kind::Show;
    return h;
}

// ==== Mirroring ===================================================================================
std::string mirrorUci(const std::string& uci) {
    std::string u = uci;
    for (size_t i : {size_t(1), size_t(3)})
        if (i < u.size() && u[i] >= '1' && u[i] <= '8') u[i] = char('1' + ('8' - u[i]));
    return u;
}

std::string mirrorFen(const std::string& fen) {
    std::vector<std::string> f = words(fen);
    if (f.size() < 4) return fen;
    std::vector<std::string> ranks = split(f[0], '/');
    std::reverse(ranks.begin(), ranks.end());
    std::string board;
    for (size_t i = 0; i < ranks.size(); ++i) {
        if (i) board += '/';
        for (char c : ranks[i]) {
            if (c >= 'a' && c <= 'z') board += char(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') board += char(c - 'A' + 'a');
            else board += c;
        }
    }
    std::string side = f[1] == "w" ? "b" : "w";
    std::string castling;
    if (f[2] == "-") {
        castling = "-";
    } else {
        // Each right moves to the other colour; FEN orders White's first.
        std::string up, low;
        for (char c : f[2]) {
            if (c >= 'a' && c <= 'z') up += char(c - 'a' + 'A');
            else low += char(c - 'A' + 'a');
        }
        castling = up + low;
    }
    std::string ep = f[3];
    if (ep.size() == 2 && ep[1] >= '1' && ep[1] <= '8') ep[1] = char('1' + ('8' - ep[1]));
    std::string out = board + " " + side + " " + castling + " " + ep;
    for (size_t i = 4; i < f.size(); ++i) out += " " + f[i];
    return out;
}

}  // namespace coach
