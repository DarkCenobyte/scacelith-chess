// Analysis mode: what the commentator says about the key moments of a reviewed game (review.h),
// as lines of the speech catalog (assets/coach/speech/<lang>/analysis.lang, rendered by
// coach::Catalog for the voice and the subtitles). Engine-free and GL-free; deterministic: the
// same review gives the same comments, wherever the player starts or how they move about.
//
// A comment belongs to a position: it is said when that position is on the board after the move
// that led to it was played (by the robots, or the player stepping forward onto it; never when
// the player steps back onto it or jumps there, see the scene). The moments:
//   - the start position: a short opening of the commentary;
//   - the position where the game leaves the opening book: the opening's name (coach/openings.h);
//   - after a blunder (??) or a mistake (?): what it allows or misses, in board facts checked
//     with coach/tactics.h (a mate allowed or missed, a piece left to be taken, a fork, a lost
//     exchange, material lost along the engine's line), then the better move; after the reply that
//     fails to punish it, the chance missed;
//   - after a brilliant (!!) or an only (!) move: why it stands out (a sacrifice, the only move
//     that holds);
//   - the first position where a side has a forced mate (the eval bar shows M#);
//   - the final position: how the game ended, and each side's accuracy and errors.
// Inaccuracies (?!) and interesting moves (!?) only show as symbols, unless a quieter stretch
// leaves room (Comment::weight 1 lines are dropped when a comment was said in the 2 positions
// before).
// The players are named by their colour ("White", "Black"), whose words the voice says well in
// every language, never by their names. Pieces are named with their side's possessive
// (coach::Arg::ofSidePiece, the catalog's owner.white.* / owner.black.* forms).
#pragma once
#include "../coach/script.h"
#include "review.h"
#include <string>
#include <vector>

namespace analysis {

struct Comment {
    int position = -1;                 // the position it is said on
    int weight = 0;                    // 3 key moment, 2 notable, 1 colour (dropped when crowded)
    std::vector<coach::Line> lines;    // the sentences, said in order
    // Squares and arrows lit (in the coach's cobalt) while it is said: the piece left hanging, the
    // fork's square, the mating move ... (coach::Mark::anchor times them on a placeholder's word).
    std::vector<coach::Mark> marks;
    bool empty() const { return lines.empty(); }
};

// What the record says of the game beyond its moves.
struct GameInfo {
    std::string result = "*";          // "1-0", "0-1", "1/2-1/2", "*"
    std::string endReasonKey;          // game/replay.h endReasonKey(): "reason.checkmate", "" = unknown
};

class Commentator {
public:
    void reset(const GameInfo& info);
    // Whether the comment of position p can be decided: the verdicts it depends on are final (the
    // plies before it within 2, and the review's summary for the final position).
    bool ready(const GameReview& review, int p) const;
    // The comment of position p (empty when nothing is worth saying there). Call once ready().
    Comment commentAt(const GameReview& review, int p) const;
    // The catalog keys every comment uses (tests: each exists in English and has the same number
    // of variants in every language).
    static std::vector<std::string> keys();

private:
    GameInfo info_;
};

}  // namespace analysis
