// Scorekeeping (FIDE art. 8.1): both players' scoresheets and what their writing hands do with
// them. Each player records every move (his own and his opponent's) on his own sheet, with the
// hand on the scoresheet side, as soon as the move is completed by the clock press. The writing
// hand runs its own animator queue, so it never delays the hand that plays (see
// anim::Animator::enqueueWriting): recording never costs clock time.
//
// Sequence of a game, driven by GameScene:
//   newGame()        blank pads, pens on the table (before the opening handshake)
//   writeHeaderInstantly()  the header, filled in before the round as in tournaments (otherwise
//                    startRecording() has the players write it, a dozen seconds of writing)
//   startRecording() both players pick up their pen
//   recordMove()     on every completed move: both players write it (turning the page first
//                    when the move starts a new page); a held sheet (setHold, hot-seat) waits,
//                    and so do both sheets from the write limit on (setWriteLimit, Coach mode)
//   finishGame()     both players write the result and lay the pen down (before the final
//                    handshake: it is made with the right hand, which may be a writing hand)
// update() once per frame after the animators, onEvent() for their events, submit() when drawing.
#pragma once
#include "../anim/animator.h"
#include "../render/renderer.h"
#include "scorekeeper_ledger.h"
#include "scoresheet.h"
#include <string>
#include <vector>

namespace game {

class Scorekeeper {
public:
    struct Player {
        std::string name;     // as written in the header ("Human", "Stockfish", the player's name)
        int elo = 0;          // 0 = left blank
        int handStyle = 0;    // ui::font::HandStyle of the sheet's owner
        bool blueInk = true;  // blue or black ballpoint
        std::string rating;   // written instead of 'elo' when set ("1500?" provisional online rating)
    };
    // Header of an online game (set after newGame(), before the header is written): the event
    // (server name / "Friendly match"), the round ("-"), and the free additions beside the
    // printed fields: the note ("Online, 5+3 rated") and the reference (game number).
    struct Details {
        std::string event, round, note, reference;
        bool noBoard = false;  // leave "Board" blank
    };
    void setDetails(const Details& d) { details_ = d; hasDetails_ = true; }

    bool init(bool clockOnPositiveX);   // GL context + ui::font ready
    void shutdown();

    // Blank pads for a new game; 'players' by seat (0 = White). The animators must have been
    // (re)initialised: their pens are on the table.
    void newGame(anim::Animator* anim, bool clockOnPositiveX, const Player players[2], int round,
                 const std::string& date);
    // Header already filled in (event, date, round, names, ratings).
    void writeHeaderInstantly();
    // Position set up without animation (--moves): the moves are already on the sheets (holds and
    // the write limit do not apply).
    void writeMovesInstantly(const std::vector<std::string>& san);
    void startRecording();
    // Move 'ply' (0 = White's first) has just been completed with the clock press.
    void recordMove(int ply, const std::string& san);
    // "1-0", "0-1" or "½-½": written after the moves still owed (holds and the write limit are
    // lifted), then the pens go back on the table.
    void finishGame(const std::string& result);
    // Pads back to blank, pens on the table, nothing queued (menu, abandoned game).
    void clear();
    // Hot-seat: while a seat is held, its writing hand starts no new entry; the moves completed
    // meanwhile wait and are written in order once it is released. The player to move records the
    // opponent's move at the start of their turn, unless they touch a piece first: it is then
    // written after their own move (FIDE 8.1.2). finishGame() releases every hold.
    void setHold(int seat, bool hold);
    bool held(int seat) const { return ledger_.held(seat); }
    // Coach mode: the moves from ply 'plies' on are recorded but neither sheet writes them yet
    // (the human's move while the coach may still take it back); -1 lifts the limit. Lifting or
    // raising it lets both sheets catch up, in order. A demonstration is never recorded at all.
    void setWriteLimit(int plies);
    int writeLimit() const { return ledger_.writeLimit(); }
    // A takeback: forgets the moves recorded from 'fromPly' on, provided neither sheet has begun
    // writing any of them (a limit or holds keep them off the sheets until then); the move played
    // instead, recorded at 'fromPly', is written normally. False (nothing changes) when a sheet
    // has begun one: ink is not erased.
    bool dropMoves(int fromPly);
    const ScoreLedger& ledger() const { return ledger_; }

    // Writing-hand events of 'seat' (sounds, entry and page bookkeeping).
    void onEvent(int seat, const anim::Event& e);
    void update();                            // after the animators' update (GL: page textures)
    void submit(render::Renderer& r);         // pads, pages and pens
    bool writing(int seat) const;             // entries or page turns still pending
    // The sheet's owner is reading it (GameScene, key S): between entries the writing hand waits
    // off the page, beside it towards its owner, instead of resting on the next row.
    void setHandAside(int seat, bool aside);
    int backlog(int seat) const;              // entries begun and not finished yet

private:
    Scoresheet::Header header() const;
    void beginMoveEntry(int seat, int ply, const std::string& san);
    void refreshRest(int seat);
    void catchUp(int seat);                   // queues the moves this sheet has not begun yet

    Scoresheet sheets_[2];
    anim::Animator* anim_ = nullptr;
    bool ready_ = false;
    bool recording_ = false;
    bool headerWritten_ = false;
    bool finished_ = false;
    Player players_[2];
    Details details_;
    bool hasDetails_ = false;
    int round_ = 1;
    std::string date_;
    ScoreLedger ledger_;                      // moves recorded, next ply of each sheet, holds, limit
    bool handAside_[2] = {false, false};
    m::mat4 prevPen_[2];
    bool hasPrevPen_[2] = {false, false};
};

// Localized piece letters for the scoresheets (key "scoresheet.pieces" = "K Q R B N").
sheet::PieceLetters localizedPieceLetters();
// Today's date as written on a scoresheet ("28.09.2026"; order per language).
std::string scoresheetDate(bool fixedForScreenshots);

}  // namespace game
