// Internal to the analysis package (review.cpp, commentary.cpp): the board facts of a move that the
// symbols and the comments both rely on, so that what the board shows (!!, !?) and what the
// commentator says about it ("it offers the knight") come from one definition. Not a public API.
#pragma once
#include "../chess/chess.h"

namespace analysis {
namespace detail {

// The material a move gives away. After it, the opponent has a legal capture whose exchange
// (coach::seePoints, in points) leaves the mover at least 2 points below its balance before the
// move, and at least 2 points more than the opponent could already win before it: a piece lost to
// a fork, or already left hanging, is not "given". The piece named is the one that capture takes
// (where it stands after the move); among several such captures, the one that costs the most.
struct Given {
    chess::Square square = chess::NoSquare;
    chess::PieceType piece = chess::NoPiece;
    int points = 0;               // what the mover is down after the opponent's capture
    bool any() const { return square != chess::NoSquare; }
};
Given materialGiven(const chess::Position& before, const chess::Move& m);

// Legal moves of the side to move that lose nothing by exchange on their square (coach::seePoints
// >= 0): the sensible answers to a check.
int sensibleMoves(const chess::Position& p);

}  // namespace detail
}  // namespace analysis
