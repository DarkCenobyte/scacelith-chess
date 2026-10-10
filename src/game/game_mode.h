// The game scene's modes (game_scene.h), apart so that engine-free code (game_saving.h, the unit
// tests) can name them without the scene.
#pragma once

namespace game {

enum class GameMode {
    Play,    // the human against Stockfish, first person
    Watch,   // viewer mode: Stockfish against Stockfish, free observer camera
    Online,  // the human against a player of the server or of a direct match, first person
    HotSeat, // two humans on this PC, in turn, each from their own robot's eyes
    Coach,   // the human against the coach (a Stockfish seat that teaches), first person
    Replay,  // a saved game (game_archive.h) played again by the robots, free observer camera
    Analysis // a game reviewed by Stockfish (src/analysis): a replay the player steps through, with
             // the evaluation bar, the move list, the symbols and arrows on the board, the comments
};

}  // namespace game
