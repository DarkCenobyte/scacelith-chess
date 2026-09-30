#include "coach/repertoire.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace coach {

namespace {

// Level bands: 0 = levels 1-2 (open games), 1 = levels 3-4 (main openings), 2 = levels 5-6 (wider mix).
int bandOf(int level) { return level <= 2 ? 0 : level <= 4 ? 1 : 2; }

struct Choice {
    const char* san;
    int weight;
};
struct Preference {
    int band;
    const char* line;              // moves from the start position, SAN, space-separated ("" = first move)
    std::vector<Choice> choices;   // weighted replies (only the legal ones that stay in book are kept)
};

// Curated choices for the positions a teaching game meets most. Keyed by position (not by move order), so a
// transposition into one of these positions gets the same answer. Positions not listed fall back to the book
// moves that lead to the most teaching lines.
const std::vector<Preference>& preferences() {
    static const std::vector<Preference> prefs = {
        // ---- Levels 1-2: the open games, which teach development, the centre and king safety.
        {0, "", {{"e4", 1}}},
        {0, "e4", {{"e5", 1}}},
        {0, "d4", {{"d5", 1}}},
        {0, "Nf3", {{"d5", 1}}},
        {0, "c4", {{"e5", 1}}},
        {0, "e4 e5", {{"Nf3", 1}}},
        {0, "e4 e5 Nf3", {{"Nc6", 1}}},
        {0, "e4 e5 Nf3 Nc6", {{"Bc4", 3}, {"d4", 1}}},
        {0, "e4 e5 Nf3 Nc6 Bc4", {{"Bc5", 3}, {"Nf6", 1}}},
        {0, "e4 e5 Nf3 Nc6 Bc4 Bc5", {{"c3", 1}, {"d3", 2}, {"O-O", 1}}},
        {0, "e4 e5 Nf3 Nc6 Bc4 Nf6", {{"d3", 3}, {"Nc3", 1}}},
        {0, "e4 e5 Nf3 Nc6 Bb5", {{"a6", 2}, {"Nf6", 1}}},
        {0, "e4 e5 Nf3 Nc6 d4", {{"exd4", 1}}},
        {0, "e4 e5 Nf3 Nc6 d4 exd4", {{"Nxd4", 1}}},
        {0, "e4 e5 Nf3 Nc6 Nc3", {{"Nf6", 1}}},
        {0, "e4 e5 Nf3 d6", {{"d4", 1}}},
        {0, "e4 e5 Nf3 Nf6", {{"Nxe5", 1}}},
        {0, "e4 e5 Bc4", {{"Nf6", 1}}},
        {0, "e4 e5 Nc3", {{"Nf6", 1}}},
        {0, "e4 e5 d4", {{"exd4", 1}}},
        {0, "e4 e5 Qh5", {{"Nc6", 1}}},
        {0, "e4 c5", {{"Nf3", 2}, {"Nc3", 1}}},
        {0, "e4 e6", {{"d4", 1}}},
        {0, "e4 c6", {{"d4", 1}}},
        {0, "e4 d5", {{"exd5", 1}}},
        {0, "e4 d5 exd5 Qxd5", {{"Nc3", 1}}},
        {0, "e4 d6", {{"d4", 1}}},
        {0, "e4 g6", {{"d4", 1}}},
        {0, "e4 Nf6", {{"e5", 1}}},
        {0, "d4 d5 c4", {{"e6", 1}, {"c6", 1}}},
        {0, "d4 d5 Nf3", {{"Nf6", 1}}},
        {0, "d4 d5 Bf4", {{"Nf6", 1}}},
        {0, "c4 e5 Nc3", {{"Nf6", 1}}},
        {0, "Nf3 d5 d4", {{"Nf6", 1}}},
        {0, "Nf3 d5 c4", {{"e6", 1}}},
        // ---- Levels 3-4: 1.e4 and 1.d4 with their main defences.
        {1, "", {{"e4", 3}, {"d4", 2}}},
        {1, "e4", {{"e5", 3}, {"c5", 2}, {"c6", 1}, {"e6", 1}}},
        {1, "d4", {{"d5", 2}, {"Nf6", 2}}},
        {1, "c4", {{"e5", 1}, {"Nf6", 1}, {"c5", 1}}},
        {1, "Nf3", {{"d5", 1}, {"Nf6", 1}}},
        {1, "e4 e5", {{"Nf3", 1}}},
        {1, "e4 e5 Nf3", {{"Nc6", 1}}},
        {1, "e4 e5 Nf3 Nc6", {{"Bb5", 2}, {"Bc4", 2}, {"d4", 1}}},
        {1, "e4 e5 Nf3 Nc6 Bb5", {{"a6", 3}, {"Nf6", 1}}},
        {1, "e4 e5 Nf3 Nc6 Bc4", {{"Bc5", 1}, {"Nf6", 1}}},
        {1, "e4 c5", {{"Nf3", 3}, {"c3", 1}}},
        {1, "e4 c5 Nf3", {{"d6", 2}, {"Nc6", 1}, {"e6", 1}}},
        {1, "e4 c5 Nf3 d6", {{"d4", 1}}},
        {1, "e4 c5 Nf3 Nc6", {{"d4", 2}, {"Bb5", 1}}},
        {1, "e4 c5 Nf3 e6", {{"d4", 1}}},
        {1, "e4 e6", {{"d4", 1}}},
        {1, "e4 e6 d4 d5", {{"Nc3", 2}, {"e5", 1}, {"Nd2", 1}}},
        {1, "e4 c6", {{"d4", 1}}},
        {1, "e4 c6 d4 d5", {{"Nc3", 2}, {"e5", 1}}},
        {1, "d4 d5", {{"c4", 3}, {"Nf3", 1}, {"Bf4", 1}}},
        {1, "d4 d5 c4", {{"e6", 2}, {"c6", 2}}},
        {1, "d4 Nf6", {{"c4", 3}, {"Nf3", 1}, {"Bf4", 1}}},
        {1, "d4 Nf6 c4", {{"e6", 2}, {"g6", 2}}},
        {1, "d4 Nf6 c4 e6", {{"Nc3", 2}, {"Nf3", 2}, {"g3", 1}}},
        {1, "d4 Nf6 c4 e6 Nc3", {{"Bb4", 1}}},
        {1, "d4 Nf6 c4 e6 Nf3", {{"b6", 1}, {"d5", 1}}},
        {1, "d4 Nf6 c4 g6", {{"Nc3", 1}}},
        {1, "d4 Nf6 c4 g6 Nc3", {{"Bg7", 2}, {"d5", 1}}},
        // ---- Levels 5-6: a wider, flatter mix, the Sicilian first against 1.e4.
        {2, "", {{"e4", 3}, {"d4", 3}, {"c4", 1}, {"Nf3", 1}}},
        {2, "e4", {{"c5", 3}, {"e5", 2}, {"e6", 1}, {"c6", 1}}},
        {2, "d4", {{"Nf6", 3}, {"d5", 2}}},
        {2, "c4", {{"e5", 1}, {"Nf6", 1}, {"c5", 1}, {"e6", 1}}},
        {2, "Nf3", {{"Nf6", 1}, {"d5", 1}}},
        {2, "e4 c5", {{"Nf3", 3}, {"c3", 1}, {"Nc3", 1}}},
        {2, "e4 c5 Nf3", {{"d6", 2}, {"Nc6", 1}, {"e6", 1}}},
        {2, "e4 e5 Nf3", {{"Nc6", 1}}},
        {2, "e4 e5 Nf3 Nc6", {{"Bb5", 3}, {"Bc4", 1}, {"d4", 1}}},
        {2, "e4 e5 Nf3 Nc6 Bb5", {{"a6", 2}, {"Nf6", 1}}},
        {2, "d4 d5", {{"c4", 1}}},
        {2, "d4 d5 c4", {{"e6", 2}, {"c6", 2}, {"dxc4", 1}}},
        {2, "d4 Nf6", {{"c4", 3}, {"Nf3", 1}}},
        {2, "d4 Nf6 c4", {{"e6", 2}, {"g6", 2}}},
    };
    return prefs;
}

// Curated choices per band, keyed by the hash of the position they answer.
const std::map<uint64_t, const Preference*>& preferenceIndex(int band) {
    static const std::vector<std::map<uint64_t, const Preference*>> index = [] {
        std::vector<std::map<uint64_t, const Preference*>> out(3);
        for (const Preference& p : preferences()) {
            chess::Position pos;
            std::string line = p.line;
            size_t i = 0;
            bool ok = true;
            while (ok && i < line.size()) {
                size_t j = line.find(' ', i);
                if (j == std::string::npos) j = line.size();
                const chess::Move m = pos.parseSAN(line.substr(i, j - i));
                if (m.valid()) pos.makeMove(m);
                else ok = false;
                i = j + 1;
            }
            if (ok) out[p.band][pos.hash()] = &p;   // a typo in the table only loses that entry (tested)
        }
        return out;
    }();
    return index[band];
}

// SplitMix64: a small, well-mixed deterministic generator.
uint64_t mix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Candidate {
    chess::Move move;
    double weight;
};

chess::Move pick(const std::vector<Candidate>& cands, uint64_t& rng) {
    double total = 0.0;
    for (const Candidate& c : cands) total += c.weight;
    if (cands.empty() || total <= 0.0) return {};
    // 53 random bits -> [0, 1)
    double r = double(mix(rng) >> 11) * (1.0 / 9007199254740992.0) * total;
    for (const Candidate& c : cands) {
        if (r < c.weight) return c.move;
        r -= c.weight;
    }
    return cands.back().move;
}

}  // namespace

chess::Move repertoireMove(const chess::Game& game, int level, uint64_t seed, const OpeningBook& book) {
    if (level <= 0 || book.empty() || game.isOver()) return {};
    if (game.startPosition().hash() != chess::Position().hash()) return {};
    const int ply = int(game.moves().size());
    if (ply >= kRepertoireMaxPly) return {};
    const chess::Position& pos = game.position();
    if (!book.lookup(pos.hash())) return {};
    const int band = bandOf(std::min(level, 6));
    uint64_t rng = seed ^ (pos.hash() * 0xD1B54A32D192ED03ull) ^ uint64_t(ply);

    // Curated first: any listed move that is legal here and stays in book (hand-picked sound moves: the defence
    // 2...Nc6 against 2.Qh5 is kept although every lichess line through it is named after White's queen raid).
    const auto& index = preferenceIndex(band);
    auto it = index.find(pos.hash());
    std::vector<Candidate> cands;
    if (it != index.end()) {
        for (const Choice& c : it->second->choices) {
            const chess::Move m = pos.parseSAN(c.san);
            if (!m.valid()) continue;
            chess::Position next = pos;
            next.makeMove(m);
            if (book.lookup(next.hash())) cands.push_back({m, double(c.weight)});
        }
        if (!cands.empty()) return pick(cands, rng);
    }

    // Otherwise the book moves that lead to teaching lines (tier 1 families only at levels 1-2), weighted by how
    // many such lines go through them: strongly towards the main lines at low levels, flatter above.
    const int maxTier = band == 0 ? 1 : 2;
    for (const chess::Move& m : pos.legalMoves()) {
        chess::Position next = pos;
        next.makeMove(m);
        const int n = book.teachingLines(next.hash(), maxTier);
        if (n <= 0) continue;
        const double w = band == 0 ? double(n) * double(n) : band == 1 ? double(n) : std::sqrt(double(n));
        cands.push_back({m, w});
    }
    return pick(cands, rng);
}

}  // namespace coach
