// The coach's review of the human's moves in a Coach game (levels 1-6; level 0 is the rules
// lesson): move classification from Stockfish figures (the lichess win-percentage model), the
// choice of an explanation backed by board facts (tactics.h), praise and takeback policy per level,
// rate limits, and the Script the director performs: lines with pointing and marks, demonstration
// moves played on the table and taken back, the takeback offer. Also the "Check!" / "Checkmate!"
// announcements (either side) and the coach's threat warnings to beginners.
//
// Lines are catalog keys of assets/coach/speech/<lang>/review.lang; every pointing target is a
// placeholder of the line (the gesture's anchor), so translations keep the timing.
//
// Engine-free: coach::Session (session.cpp) runs the analyses this file asks for (Reviewer::*Request)
// through the coach::Analyst interface (stage.h) and hands the results back as ai::Analysis values;
// tests build those by hand.
#pragma once
#include "assessment.h"
#include "../ai/analysis.h"
#include "../chess/chess.h"
#include "script.h"
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace coach {

// ---- Win percentage and move classes -------------------------------------------------------------
// Lichess: W% = 50 + 50 * (2 / (1 + e^(-0.00368208 cp)) - 1), cp clamped to +-1000; mates count
// as +-1000 cp. Scores are the mover's view (Stockfish reports the side to move's).
double winPercent(int cp);
double winPercent(const ai::Score& s);
// Lichess per-move accuracy (with its +1 "uncertainty bonus"), 0..100; 100 when nothing was lost.
double moveAccuracy(double wBefore, double wAfter);

enum class MoveClass : uint8_t { Unjudged, Book, Forced, Best, Excellent, Good, Inaccuracy, Mistake, Blunder };
enum class MateChange : uint8_t {
    None,
    Created,   // the best line did not lose to a mate, the played one does
    Lost,      // the best line mated, the played one does not
    Delayed    // both mate, the played one later
};
const char* moveClassName(MoveClass c);   // "best", "blunder", ... (logs, tests)

struct Judgement {
    MoveClass cls = MoveClass::Unjudged;
    MateChange mate = MateChange::None;
    double wBest = 50.0, wPlayed = 50.0;   // mover's view, same root
    double delta = 0.0;                    // max(0, wBest - wPlayed), W% points
};
// Classes from Δ (Best < 0.5 or the engine's move, Excellent < 2, Good < 5, Inaccuracy < 10,
// Mistake < 15, Blunder >= 15) and the lichess mate rules (a created or lost mate is a blunder,
// softened when the other side of it was already clearly decided).
Judgement judge(const ai::Score& best, const ai::Score& played, bool playedIsBest);
MoveClass classifyDelta(double delta, bool playedIsBest);

// ---- Explanation types (in the order they are tried) ---------------------------------------------
enum class ExType : uint8_t {
    None,
    MateAllowed,
    MateMissed,
    Stalemate,
    Fork,
    Discovered,
    Skewer,
    Pin,
    Trapped,
    BackRank,
    Hanging,
    Exchange,
    MissedCapture,
    MissedFork,     // the human's fork not played
    PromotionRace,
    KingSafety,
    BadTrade,
    Opening,
    Endgame,
    Positional,
    Material        // material lost along the line, no named motif (after Positional: stored values stay)
};
const char* exTypeName(ExType t);   // "fork", "hanging", ...: key part of ex.<name>.b<n> and theme.<name>

// ---- Per-ply verdicts, collected for the end-of-game appraisal ----------------------------------
struct PlyVerdict {
    int ply = -1;                    // index in Game::moves() (0 = White's first move)
    chess::Color mover = chess::White;
    bool human = false;              // the listener's move
    std::string uci, san;
    MoveClass cls = MoveClass::Unjudged;
    double wBest = 50.0, wPlayed = 50.0;   // mover's view, W%
    double delta = 0.0;
    double accuracy = 100.0;         // moveAccuracy(wBest, wPlayed)
    int cpWhiteAfter = 0;            // evaluation after the move, White's view, mates +-1000
    bool hasEvalAfter = false;
    int cpWhiteBefore = 0;           // evaluation of the position before the move (best line), same scale
    bool hasEvalBefore = false;
    std::string bestSan, bestUci;    // the engine's move
    uint8_t phase = 0;               // 0 opening, 1 middlegame, 2 endgame
    ExType exType = ExType::None;    // the explanation chosen (voiced or not)
    int materialSwing = 0;           // points won (+) or lost (-) along the played line within 3 plies
    int captured = 0;                // points this move captured
    bool check = false;
    bool only = false, brilliant = false, great = false, goodCapture = false;
    bool mateMissed = false, mateAllowed = false;
    bool hastensMate = false;        // lost to a mate either way, but sooner than after the best move
    bool voiced = false, offered = false, praised = false;
};

struct TakebackRecord {
    int ply = -1;
    std::string firstUci;            // the move taken back
    ExType firstType = ExType::None;
    bool fixed = false;              // the replacement lost less than 5 W% points
    bool same = false;               // the same move was played again
    // A blunder offered back without its reason or the better move ("your knight had a better
    // move"): that move, said once the offer is declined; 'found': the replacement is that move.
    bool hintOnly = false;
    // A dubious move offered back at levels 1-2 ("maybe you can do better?"): nothing said when the
    // offer is answered; 'found' as for hintOnly.
    bool dubious = false;
    std::string bestUci;
    bool found = false;
};

// ---- Level parameters ---------------------------------------------------------------------------
struct Band {
    int level = 1;
    // Levels 1-3 (beginners): a demonstration plays the whole sequence, until the material is lost
    // for good or the king is mated. Levels 4-6: it plays up to the position where the problem
    // stands on the board (the fork, the pin, the move before the mate) and the coach points at
    // what falls next, without playing the last capture.
    bool fullDemo = true;
    int demoPlies = 1;          // longest demonstration moved on the table (either kind)
    int linePlies = 1;          // longest line said in words ({line}, the better move's idea)
    int mateLinePlies = 1;      // longest mating line shown in full (M1 = 1 ply, M2 = 3, M3 = 5)
    int missedMateMax = 1;      // missed mates voiced up to this many moves
    int lookahead = 2;          // plies of the refutation searched for a concrete cause
    int sentences = 3;          // Say lines per explanation (demo narration, rewind and offer excluded)
    int remarksPer10 = 5;       // unsolicited remarks (praise, tips, mistakes) per 10 human moves
    int praiseEvery = 3;        // at most one praise per this many human moves
    int offerCap = -1;          // takebacks the player may accept per game (-1: no cap)
    float demoPause = 0.8f;     // pause after a demonstration move lands
};
Band band(int level);           // clamped to 1..6

// ---- The review --------------------------------------------------------------------------------
struct ReviewInput {
    const chess::Game* game = nullptr;       // the human's move is the last one played
    const ai::Analysis* before = nullptr;    // A0: MultiPV 3 on the position before the move
    const ai::Analysis* played = nullptr;    // A1: the played move re-scored at the same root (needsPlayedRequest)
    const ai::Analysis* after = nullptr;     // A2 (optional): the position after the move, for a longer refutation
    const ai::Analysis* shallow = nullptr;   // A3 (optional): a shallow search of the same root ("tricky" praise)
    bool inBook = false;                     // the position after the move is in the openings book
    // The searches of the sequence a reason would show, by key (Reviewer::verifyRequest). Set, the
    // review stands only by what they confirm move by move: the player's defences in the sequence
    // are the best ones (another as good that loses less makes the claim smaller, or no claim), the
    // coach's moves are not weaker than the engine's there. A search the review still wants is
    // Review::verifyNext. Null: the sequences are the engine's lines as they come (tests, tools that
    // do not verify).
    const std::map<std::string, ai::Analysis>* verified = nullptr;
};

// What a voiced reason asserts about the move, for audits (tools/coach_audit): its type, the points
// the human loses for good by its sequence (0: a reason that is not about material), the plies of the
// refutation (from the position after the move) that sequence takes, and whether some of them are
// replies the engine did not show (guessed from the board: extendRefutation).
struct Claim {
    ExType type = ExType::None;
    int lost = 0;
    int plies = 0;
    std::vector<std::string> line;   // UCI, the first 'plies' moves of the refutation
    bool guessed = false;
    bool mate = false;
    // The common checks' figures (claims.cpp): the human's free moves inside the sequence, the
    // centipawns by which the engine's drop falls short of the material lost, whether the move's own
    // deeper line loses as much; what the best move loses and the move's own line, and the drop.
    int quiet = 0, compensation = 0;
    bool ownLine = true;
    int held = -1;   // the loss that stands along the line (lineLoss from its first ply)
    int bestLoss = 0, playedLoss = -1, dropCp = 0;
    // The verification (ReviewInput::verified): the plies of the sequence confirmed move by move,
    // whether it changed the engine's line (a better defence or a stronger move found on the way),
    // and what it did to the reason first found: 0 kept, 1 made smaller (a defence that loses less),
    // 2 dropped (not confirmed, or nothing left to claim).
    int verifiedPlies = -1;
    bool lineChanged = false;
    int reduced = 0;
};

struct Review {
    Script script;                // empty when the coach says nothing
    PlyVerdict verdict;           // store it for the appraisal (Appraisal::addReview)
    bool offersTakeback = false;  // the script ends with an OfferTakeback beat
    bool isRetry = false;         // this move replaced a taken-back one: 'takeback' describes it
    TakebackRecord takeback;
    Claim claim;                  // the reason said (type None when none is)
    Assessment assessment;        // what the move does to the expected result (assessment.h)
    // With ReviewInput::verified: the key of the next search the review wants (verifyRequest) before
    // it can stand by its reason, "" when it wants none. A review given without it says no reason it
    // could not confirm.
    std::string verifyNext;
    Claim dropped;                // the reason first found, when the verification took it back or made it smaller (audits)
    bool unsettled = false;       // a fault the search's last iterations disagree on (said at most as a mistake, no reason)
    double costBefore[2] = {-1.0, -1.0};   // what the fault cost at the two iterations before the last (-1: not known)
    bool ignoredThreat = false;   // the verdict says the move left the coach's threat standing (levels 1-2)
    // The refutation (UCI, from the position after the move) the reason was looked for on, when the
    // verification changed the engine's line (audits); empty otherwise.
    std::vector<std::string> refutation;
    std::vector<std::string> verifyLog;   // what the verification did, round by round (audits)
};

class Reviewer {
public:
    // A new Coach game. offersEnabled false: judge and explain, never offer (engine or mode limits).
    void reset(int level, chess::Color human, bool offersEnabled = true);
    int level() const { return level_; }
    chess::Color human() const { return human_; }

    // Analyses for a review (full strength; the coach's play settings do not apply):
    // A0 when the human's turn begins (MultiPV 3 on the current position)...
    ai::AnalysisRequest beforeRequest(const chess::Game& g) const;
    // ...A1 once the human's move is on the Game, only when A0 does not hold the played move or
    // holds only a bound for it, its best line aside (same root, same depth, searchmoves = the move)...
    static bool needsPlayedRequest(const ai::Analysis& before, const std::string& playedUci);
    ai::AnalysisRequest playedRequest(const chess::Game& g, const ai::Analysis& before) const;
    // ...and A2 (optional) on the position after the move, for a refutation longer than A0's PV.
    ai::AnalysisRequest afterRequest(const chess::Game& g) const;
    // A3 (optional, levels 3-4): a shallow search (depth 6) of A0's root, requested with A0. When its
    // move differs from the move played (a top move), that move was "not easy to see" (praise at
    // levels 3-4).
    ai::AnalysisRequest shallowRequest(const chess::Game& g) const;
    // A search the review asks for before it stands by the sequence it would show (Review::verifyNext,
    // review_internal.h: compareKey): a position's FEN, searched at depth kVerifyDepth (MultiPV 3), or
    // "FEN | m1 m2", the two moves compared at depth kCompareDepth; each capped in time. The session
    // runs them one after another, each time asking probe() for the next, at most kVerifyMax per
    // review.
    static ai::AnalysisRequest verifyRequest(const std::string& key);
    static constexpr int kVerifyDepth = 12;
    static constexpr int kCompareDepth = 16;
    static constexpr int kVerifyMax = 10;
    // A fault is unsettled when one of the two iterations before the search's last found the move
    // costing less than this (win-% points): a good move then (review.cpp).
    static constexpr double kUnsettledCost = 5.0;
    // The review this input would give, the reviewer left as it is (rate limits, offers, takeback):
    // for Review::verifyNext before the review itself.
    Review probe(const ReviewInput& in) const {
        Reviewer copy = *this;
        return copy.review(in);
    }

    // Right after any move is completed (either side): "Check!", "Checkmate!" (Urgent beats).
    Script announce(const chess::Game& g);

    // The human's move (the last of the Game) judged and explained. Call once per human move, after
    // announce(). Unjudged when A0 is missing or failed (engine unavailable): rules-based only.
    Review review(const ReviewInput& in);

    // After the coach's own move, before the human plays: warnings about the coach's new threats
    // (levels 1-2) and, at level 1, "my last move was a mistake" once A0 of the human's turn
    // ('before', optional) confirms the capture. Low-priority beats. 'threat' (optional): the search
    // of threatKey(g), what the coach would play with the turn passed to it: a warning the board's
    // rules find is said only when that search wins the material (or mates) too, and the threat is
    // kept for the review of the player's move ("you did not answer my threat").
    Script coachMoved(const chess::Game& g, const ai::Analysis* before = nullptr, const ai::Analysis* threat = nullptr);
    // The search of the coach's threats after its move (levels 1-2): the position with the turn passed
    // to the coach, searched as a verification's short search (verifyRequest(key)); "" when there is
    // none to search (levels 3-6, the player in check, the game over, the last move the player's).
    std::string threatKey(const chess::Game& g) const;

    // The player's answer to an offer. Accepted: call after Game::undo(1); returns the hint
    // (levels 1-2) and remembers the move so the replay is judged against it. Declined: returns the
    // better move when the offer did not name it (a blunder without a reason: TakebackRecord::
    // hintOnly), said before the director's events line; nothing is remembered.
    // fromOffer: the player took the card. The pause menu's takeback of an offered move passes
    // false: the replay is judged the same way, but the level's takebacks (Band::offerCap) count
    // only those the coach offered and the player accepted.
    Script takebackAccepted(const chess::Game& g, bool fromOffer = true);
    Script takebackDeclined();

    int offersMade() const { return offers_; }
    int humanMoves() const { return humanMoves_; }
    // Levels 4-6: the takebacks the player may still accept this game (Band::offerCap less those
    // accepted; an offer about a mate is made and taken back whatever the count). -1 without a cap
    // (levels 1-3) or without offers.
    int takebacksLeft() const;
    // The last offer, accepted, uses one of those takebacks (a capped level, not a mate's offer).
    bool offerUsesTakeback() const;

private:
    int level_ = 1;
    chess::Color human_ = chess::White;
    bool offersEnabled_ = true;
    int humanMoves_ = 0;              // human moves reviewed (retries excluded)
    int lastPraise_ = -1000;          // humanMoves_ at the last praise
    int lastSlip_ = -1000;            // humanMoves_ at the last inaccuracy remarked on
    std::deque<int> remarks_;         // humanMoves_ of each unsolicited remark (sliding window of 10)
    int offers_ = 0;
    int takebacks_ = 0;               // offers accepted that count against Band::offerCap
    bool noneLeftSaid_ = false;       // "no more takebacks this game" (once per game)
    bool brilliantDone_ = false;
    int bookPraises_ = 0;
    bool castlePraised_ = false;
    bool coachCheckExplained_ = false;
    bool punishHintDone_ = false;
    // The coach's threat after its last move, as the threat search found it (coachMoved): the move it
    // would play, what it wins for good within four plies, a mate; ply: of the coach's move (-1: none).
    struct Threat {
        int ply = -1;
        std::string uci;
        int gain = 0;
        bool mate = false;
    };
    Threat threat_;
    uint32_t tipsSaid_ = 0;           // opening principles and endgame tips already voiced (bits)
    std::vector<double> humanW_;      // the human's W% after each judged move on the board (index = ply, -1: none)
    // The last offer, and a takeback in progress.
    TakebackRecord offered_;          // the move the last offer was about
    chess::Square offeredHint_ = chess::NoSquare;
    Beat offeredReveal_;              // the better move, said when a hint-only offer is declined
    int offersAtPly_ = 0;             // offers made for offered_.ply
    bool retryPending_ = false;
    TakebackRecord retry_;

    bool remarkAllowed() const;
    void noteRemark();
};

}  // namespace coach
