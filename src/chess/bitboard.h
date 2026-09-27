// Internal to src/chess: bitboard helpers and compile-time attack / Zobrist tables.
// Everything here is constexpr (read-only, no initialisation order or thread-safety concerns).
#pragma once
#include <cstdint>

namespace chess {
namespace bb {

using U64 = uint64_t;

constexpr U64 bit(int s) { return U64(1) << s; }
inline int lsb(U64 b) { return __builtin_ctzll(b); }
inline int msb(U64 b) { return 63 ^ __builtin_clzll(b); }
inline int popLsb(U64& b) {
    int s = __builtin_ctzll(b);
    b &= b - 1;
    return s;
}
inline int popcount(U64 b) { return __builtin_popcountll(b); }
inline bool several(U64 b) { return (b & (b - 1)) != 0; }

constexpr U64 kFileA = 0x0101010101010101ULL;
constexpr U64 kRank1 = 0xFFULL;
constexpr U64 fileBB(int f) { return kFileA << f; }
constexpr U64 rankBB(int r) { return kRank1 << (8 * r); }
constexpr U64 kLightSquares = 0x55AA55AA55AA55AAULL;  // b1, d1, ..., a2, c2, ... (a1 is dark)
constexpr U64 kDarkSquares = ~kLightSquares;

// Ray directions. The first four increase the square index (first blocker = lsb), the last four
// decrease it (first blocker = msb).
enum Dir { N = 0, E = 1, NE = 2, NW = 3, S = 4, W = 5, SE = 6, SW = 7 };
constexpr int kDirFile[8] = {0, 1, 1, -1, 0, -1, 1, -1};
constexpr int kDirRank[8] = {1, 0, 1, 1, -1, 0, -1, -1};

struct Tables {
    U64 knight[64] = {};
    U64 king[64] = {};
    U64 pawn[2][64] = {};      // squares attacked by a pawn of colour [c] standing on [s]
    U64 ray[8][64] = {};       // empty-board rays
    U64 between[64][64] = {};  // squares strictly between two aligned squares (0 otherwise)
    uint8_t castleMask[64] = {};
};

constexpr bool onBoard(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

constexpr Tables buildTables() {
    Tables t{};
    const int kn[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    for (int s = 0; s < 64; ++s) {
        const int f = s & 7, r = s >> 3;
        for (int k = 0; k < 8; ++k) {
            const int ff = f + kn[k][0], rr = r + kn[k][1];
            if (onBoard(ff, rr)) t.knight[s] |= bit(rr * 8 + ff);
        }
        for (int d = 0; d < 8; ++d) {
            int ff = f + kDirFile[d], rr = r + kDirRank[d];
            if (onBoard(ff, rr)) t.king[s] |= bit(rr * 8 + ff);
            U64 ray = 0;
            while (onBoard(ff, rr)) {
                ray |= bit(rr * 8 + ff);
                ff += kDirFile[d];
                rr += kDirRank[d];
            }
            t.ray[d][s] = ray;
        }
        if (onBoard(f - 1, r + 1)) t.pawn[0][s] |= bit((r + 1) * 8 + f - 1);
        if (onBoard(f + 1, r + 1)) t.pawn[0][s] |= bit((r + 1) * 8 + f + 1);
        if (onBoard(f - 1, r - 1)) t.pawn[1][s] |= bit((r - 1) * 8 + f - 1);
        if (onBoard(f + 1, r - 1)) t.pawn[1][s] |= bit((r - 1) * 8 + f + 1);
        // Castling rights lost when a move starts or ends on these squares.
        uint8_t mask = 0xF;
        if (s == 0) mask = uint8_t(~2 & 0xF);         // a1: white queen side
        if (s == 4) mask = uint8_t(~(1 | 2) & 0xF);   // e1
        if (s == 7) mask = uint8_t(~1 & 0xF);         // h1: white king side
        if (s == 56) mask = uint8_t(~8 & 0xF);        // a8: black queen side
        if (s == 60) mask = uint8_t(~(4 | 8) & 0xF);  // e8
        if (s == 63) mask = uint8_t(~4 & 0xF);        // h8: black king side
        t.castleMask[s] = mask;
    }
    for (int a = 0; a < 64; ++a) {
        for (int d = 0; d < 8; ++d) {
            int ff = (a & 7) + kDirFile[d], rr = (a >> 3) + kDirRank[d];
            U64 acc = 0;
            while (onBoard(ff, rr)) {
                const int b = rr * 8 + ff;
                t.between[a][b] = acc;
                acc |= bit(b);
                ff += kDirFile[d];
                rr += kDirRank[d];
            }
        }
    }
    return t;
}

inline constexpr Tables kTables = buildTables();

inline U64 slide(int d, int s, U64 occ) {
    U64 a = kTables.ray[d][s];
    const U64 blockers = a & occ;
    if (blockers) a ^= kTables.ray[d][d < 4 ? lsb(blockers) : msb(blockers)];
    return a;
}
inline U64 rookAttacks(int s, U64 occ) { return slide(N, s, occ) | slide(E, s, occ) | slide(S, s, occ) | slide(W, s, occ); }
inline U64 bishopAttacks(int s, U64 occ) { return slide(NE, s, occ) | slide(NW, s, occ) | slide(SE, s, occ) | slide(SW, s, occ); }
inline U64 rookRays(int s) { return kTables.ray[N][s] | kTables.ray[E][s] | kTables.ray[S][s] | kTables.ray[W][s]; }
inline U64 bishopRays(int s) { return kTables.ray[NE][s] | kTables.ray[NW][s] | kTables.ray[SE][s] | kTables.ray[SW][s]; }

// ---- Zobrist keys (fixed seed: hashes are reproducible across runs and platforms) ----------
struct Zobrist {
    U64 piece[2][7][64] = {};
    U64 castling[16] = {};
    U64 epFile[8] = {};
    U64 side = 0;
};

constexpr U64 splitmix64(U64& state) {
    state += 0x9E3779B97F4A7C15ULL;
    U64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

constexpr Zobrist buildZobrist() {
    Zobrist z{};
    U64 state = 0x5CACE117C4E55ULL;
    for (int c = 0; c < 2; ++c)
        for (int t = 1; t < 7; ++t)
            for (int s = 0; s < 64; ++s) z.piece[c][t][s] = splitmix64(state);
    U64 rights[4] = {};
    for (int i = 0; i < 4; ++i) rights[i] = splitmix64(state);
    for (int m = 0; m < 16; ++m)
        for (int i = 0; i < 4; ++i)
            if (m & (1 << i)) z.castling[m] ^= rights[i];
    for (int f = 0; f < 8; ++f) z.epFile[f] = splitmix64(state);
    z.side = splitmix64(state);
    return z;
}

inline constexpr Zobrist kZobrist = buildZobrist();

}  // namespace bb
}  // namespace chess
