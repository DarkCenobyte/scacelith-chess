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
//   - the final position: how the game ended, and each side's accuracy and errors (those once the
//     whole review is final: until then they come apart, summaryAt()).
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
    // The positions whose final evaluations the comment of position p still waits for, the nearest
    // first: among p - 3 .. p (the move, the opponent's move before it for a chance missed, and the
    // comments of the two positions before for the crowding rule), those neither final nor failed.
    // The review's urgent searches while that comment waits (GameReview::nextRequest). Empty for
    // the start position (its words need no evaluation) and outside the game.
    std::vector<int> needs(const GameReview& review, int p) const;
    // Whether the comment of position p can be decided: needs() is empty. Never the whole review,
    // the final position's included (its accuracy account comes apart, summaryAt()).
    bool ready(const GameReview& review, int p) const;
    // The comment of position p (empty when nothing is worth saying there). Call once ready(). The
    // final position's says how the game ended, and each side's accuracy and errors only when the
    // whole review is final (GameReview::complete()): never from provisional evaluations.
    Comment commentAt(const GameReview& review, int p) const;
    // The final position's accounts of accuracy and errors alone, for a comment of the final
    // position decided before the review was complete; empty until GameReview::complete().
    Comment summaryAt(const GameReview& review) const;
    // The catalog keys every comment uses (tests: each exists in English and has the same number
    // of variants in every language).
    static std::vector<std::string> keys();

private:
    GameInfo info_;
};

// The comment the board waits for (the scene's; engine-free so that the tests drive it with the
// review): a forward step onto a position, or the welcome, asks for its comment. It is kept as long
// as that position stays on the board, however long the review takes, and handed out exactly once,
// when the commentator can decide it (Commentator::ready); a step elsewhere drops it. Its needs are
// the review's urgent searches meanwhile. The final position's comment decided before the review
// is complete leaves its accounts of accuracy and errors waiting the same way, for the end of the
// review (and of the comment being said): said once, if the final position is still on the board.
class CommentWait {
public:
    void ask(int position);          // a forward step reached it (the welcome: 0)
    void clear();                    // a step elsewhere, the comments switched off
    bool waiting() const { return pos_ >= 0; }   // a comment or the final accounts are to come
    bool waitingComment() const { return pos_ >= 0 && comment_; }   // the comment itself (Play waits)
    int position() const { return pos_; }       // -1 when nothing waits
    // The positions to search first meanwhile (Commentator::needs of the comment, nearest first).
    std::vector<int> urgent(const GameReview& review, const Commentator& c) const;
    // Once a frame, with the position on the board and whether a comment is being said: true with
    // the comment to say now. A comment decided with nothing worth saying ends the wait silently.
    bool poll(const GameReview& review, const Commentator& c, int board, bool speaking, Comment& out);

private:
    int pos_ = -1;
    bool comment_ = false;   // the comment of pos_ is still to come
};

}  // namespace analysis
