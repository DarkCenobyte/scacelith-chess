// The coach's script: what the Coach-mode director (game side) performs, beat by beat. The coach's
// brain (review, lesson, appraisal, announcements) turns engine facts into a Script; the director
// speaks each line through the catalog and the TTS, moves the coach's hand and head, lights the
// marks, plays demonstration moves on the table and takes them back, all in order.
//
// Engine-free value types: every producer and the director agree on them, and tests build them
// directly.
#pragma once
#include "../chess/chess.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coach {

// Typed argument of a catalog line. The catalog renders it twice: written (subtitles: "e4", figurine
// SAN "♘f3", digits) and spoken (TTS: squares and moves as words, numbers where the voice stumbles).
struct Arg {
    enum class Kind : uint8_t { Piece, Square, Move, Number, Text, Opening, Moves, Eval } kind = Kind::Text;
    chess::PieceType piece = chess::NoPiece;
    chess::Color color = chess::White;
    bool own = false;                          // Piece: belongs to the listener (the human): "your" forms
    chess::Square square = chess::NoSquare;    // Piece: where it stands (for pointing); Square: the square
    std::string san, uci;                      // Move (san without figurines; the catalog adds them);
                                               // Moves: 'san' holds a line, space-separated SAN ("Nxe5 dxe5 Qg4")
    int number = 0;                            // Number; Eval: centipawns, listener's point of view
    int mate = 0;                              // Eval: mate in N moves (> 0 the listener mates, < 0 is mated)
    std::string text;                          // Text: a catalog key rendered in place (or literal text
                                               // when it is no key); Opening: the opening's catalog id

    static Arg ofPiece(chess::PieceType t, chess::Color c, bool own, chess::Square on = chess::NoSquare) {
        Arg a; a.kind = Kind::Piece; a.piece = t; a.color = c; a.own = own; a.square = on; return a;
    }
    static Arg ofSquare(chess::Square s) { Arg a; a.kind = Kind::Square; a.square = s; return a; }
    static Arg ofMove(const std::string& san, const std::string& uci) {
        Arg a; a.kind = Kind::Move; a.san = san; a.uci = uci; return a;
    }
    static Arg ofNumber(int n) { Arg a; a.kind = Kind::Number; a.number = n; return a; }
    static Arg ofText(const std::string& keyOrText) { Arg a; a.kind = Kind::Text; a.text = keyOrText; return a; }
    static Arg ofOpening(const std::string& id) { Arg a; a.kind = Kind::Opening; a.text = id; return a; }
    static Arg ofMoves(const std::string& sanLine) { Arg a; a.kind = Kind::Moves; a.san = sanLine; return a; }
    static Arg ofEval(int centipawns, int mateIn = 0) {
        Arg a; a.kind = Kind::Eval; a.number = centipawns; a.mate = mateIn; return a;
    }
};

// One message of the catalog: a key ("ex.fork.b2"; the renderer picks one of its variants) and the
// values of its placeholders ("{your}" -> args named "your").
struct Line {
    std::string key;
    std::vector<std::pair<std::string, Arg>> args;

    bool empty() const { return key.empty(); }
    Line& with(const std::string& name, const Arg& a) { args.emplace_back(name, a); return *this; }
    const Arg* arg(const std::string& name) const {
        for (const auto& p : args) if (p.first == name) return &p.second;
        return nullptr;
    }
};

enum class GestureKind : uint8_t {
    PointSquare,  // index finger at 'square'
    PointPiece,   // index finger at the piece standing on 'square'
    Trace,        // fingertip along 'path' (from, [corner], to): a move or a line of attack
    Present,      // open palm towards 'square' (NoSquare: the board as a whole)
    Beat,         // rhythmic down-strokes that stress the words
    Open,         // open palm towards the listener: a question, an invitation
    Nod,          // head: yes, well done
    ShakeHead     // head: no, careful
};

// A gesture lands on the spoken word it refers to: 'anchor' names a placeholder of the line ("sq",
// "your", or a zero-width "{@}" / "{@2}" marker of static lines, named "@" / "@2"); the director
// estimates when that word is heard (the catalog returns its character offset in the spoken text)
// and times the apex just before it. Without an anchor, 'at' is a fraction 0..1 of the line's audio.
struct Gesture {
    GestureKind kind = GestureKind::PointSquare;
    chess::Square square = chess::NoSquare;
    std::vector<chess::Square> path;           // Trace: from, [corner], to (knight: the L's corner square)
    std::string anchor;
    float at = 0.0f;
    bool emphasis = false;                     // Point: two small jabs on arrival
};

// A highlight on the board while the beat runs: switched on with the gesture it belongs to (or at
// the start of the line), off shortly after the line, or kept until the next Rewind beat.
struct Mark {
    enum class Kind : uint8_t { Square, Piece, Arrow } kind = Kind::Square;
    chess::Square square = chess::NoSquare;    // Square, Piece (the piece standing on it)
    chess::Square from = chess::NoSquare, via = chess::NoSquare, to = chess::NoSquare;  // Arrow (via: knight corner)
    std::string anchor;                        // switch on with this placeholder's word ("" = line start)
    bool untilRewind = false;                  // keep it lit through the following demonstration
};

// Where the coach looks while a beat runs (the gaze rule: at what it talks about, else at the
// player). Target = the first gesture's or mark's square, following each gesture in turn.
enum class Look : uint8_t { Player, Board, Target };

enum class BeatKind : uint8_t {
    Say,            // speak 'line' with its gestures, marks and look
    DemoMove,       // play 'uci' on the table by hand (either colour), never recorded; 'line' (optional)
                    // is narrated from the move's reach so that the piece lands near the phrase end
    Rewind,         // take the last 'count' demonstration moves back by hand, one ply at a time
    OfferTakeback,  // speak 'line' (the offer) and show the takeback card; the director handles the
                    // answer (the beats after it are dropped when the player takes back)
    Pause,          // silence for 'seconds'
    WaitMove,       // lesson: wait until the player plays a move satisfying expectation 'expect'
    SetPosition     // lesson: set up 'fen' for the next exercise (behind a short fade)
};

// Turn-taking (research-pedagogy §6.5): Urgent may cut a running Normal/Low line at its next sentence
// boundary; Low lines are dropped when stale (more than a few plies old) or once the player acts.
enum class Priority : uint8_t { Urgent = 1, Normal = 2, Low = 3 };

struct Beat {
    BeatKind kind = BeatKind::Say;
    Line line;
    std::vector<Gesture> gestures;
    std::vector<Mark> marks;
    Look look = Look::Player;
    std::string uci;           // DemoMove
    int count = 0;             // Rewind
    float seconds = 0.0f;      // Pause
    std::string fen;           // SetPosition
    int expect = -1;           // WaitMove: index into the lesson's expectations
    bool skippable = true;     // Space skips it (a Rewind is never skipped, it only goes faster)
    Priority priority = Priority::Normal;
    int ply = -1;              // game ply the beat is about (-1: none); stale Low beats are dropped
};

using Script = std::vector<Beat>;

}  // namespace coach
