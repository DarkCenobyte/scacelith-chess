// What the coach needs from the world around it: the Stage (voice, subtitles, body, table, HUD)
// and the Analyst (Stockfish analyses). The game scene implements both (game_scene_coach.cpp);
// the tests implement fakes. The director and the session (coach/director.h, coach/session.h)
// only talk to the world through these two interfaces, so they stay engine-free and GL-free.
//
// Every call returns at once. Long actions (a gesture, a demonstration move, a rewind) run in the
// scene; the director polls busy() / the voice clock to know when they are over.
#pragma once
#include "script.h"
#include "../ai/analysis.h"
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

class Stage {
public:
    virtual ~Stage() = default;

    // ---- Voice (the TTS worker and the audio engine's speech voice) -----------------------------
    // False when the coach cannot be heard (voice files missing, TTS failed to load, no audio
    // device): the director then shows every line as a subtitle held for readingTime().
    virtual bool voiceAvailable() const = 0;
    // Queue the synthesis of 'text' (spoken rendering) in 'lang' (a Supertonic language) at 'speed'
    // (1 = normal, the lesson uses kLessonSpeechSpeed). Higher priority first. 0 = refused.
    virtual uint32_t requestSpeech(const std::string& text, const std::string& lang, float speed, int priority) = 0;
    // The synthesised audio (mono float PCM at speechSampleRate()) once ready: true once, then the
    // request is forgotten. False while pending.
    virtual bool takeSpeech(uint32_t request, std::vector<float>& pcm) = 0;
    virtual bool speechFailed(uint32_t request) const = 0;   // the request will never be ready
    virtual void cancelSpeech(uint32_t request) = 0;          // 0 = every queued request
    virtual int speechSampleRate() const = 0;                 // 44100
    // Play one utterance from the coach's mouth (one at a time). False when the audio engine refused
    // it this frame: the director keeps the PCM and retries on the next update.
    virtual bool startVoice(std::vector<float>&& pcm) = 0;
    virtual void stopVoice() = 0;                             // short fade; the utterance is over
    virtual void pauseVoice(bool paused) = 0;
    // Seconds of the current utterance heard so far (the speech clock, output latency removed), or a
    // negative value when no utterance plays. *finished is set once it has been heard to the end
    // (or was stopped or dropped by the audio engine).
    virtual double voiceClock(bool* finished) const = 0;

    // ---- Subtitles --------------------------------------------------------------------------------
    // Show the written rendering of the line being said for 'holdSeconds' ("" hides the subtitle).
    virtual void showSubtitle(const std::string& written, float holdSeconds) = 0;
    // How long a subtitle stays when nothing is heard (ui::subtitleDuration(text, 0)).
    virtual float readingTime(const std::string& written) const = 0;

    // ---- The coach's body -------------------------------------------------------------------------
    // Gaze for the beat that starts: at the player's face, over the board, or at 'target' (the
    // scene then follows the gestures' own gaze, as the animator does).
    virtual void look(Look look, chess::Square target) = 0;
    // Perform a gesture whose apex (the finger's arrival, a palm's arrival, the first stroke, the
    // nod) should land 'apexIn' seconds from now (0 = as soon as possible). Gestures of one beat are
    // given in order and chain in the animator.
    virtual void gesture(const Gesture& g, float apexIn) = 0;
    // The line is over: end the running hold and retract the hand to rest.
    virtual void endGestures() = 0;
    // The mouth moves with the voice level (0 when silent); called every frame by the director.
    virtual void speechLevel(float level) = 0;

    // ---- The table ----------------------------------------------------------------------------------
    // Demonstrate 'uci' by hand from the position on the table (either colour; captured pieces go
    // beside the board), never recorded in the game nor on a scoresheet. 'pause' seconds of stillness
    // after the piece is placed.
    virtual void demoMove(const std::string& uci, float pause) = 0;
    // Take the last 'plies' demonstration moves back by hand, one ply at a time (moved piece first,
    // then the captured one). fast: Space was pressed, move briskly.
    virtual void rewindDemo(int plies, bool fast) = 0;
    // Take real moves of the game back: Game::undo, the pieces back by hand, the scoresheets
    // (Scorekeeper::dropMoves). Used for an accepted takeback offer and for the lesson's reactions.
    virtual void takeBack(int plies) = 0;
    // Rules lesson: set up 'fen' (fade, physical re-sync, game reset from the FEN).
    virtual void setPosition(const std::string& fen) = 0;
    // Rules lesson: the coach plays 'uci' by hand as a real move of the lesson game (no scoresheet).
    virtual void playLessonMove(const std::string& uci) = 0;
    // A demonstration, rewind, takeback, set-up or lesson move is still running. True from the
    // call that starts it on (the director polls it on the following frames).
    virtual bool tableBusy() const = 0;
    // Space during a rewind: the rewind already running goes on briskly (as rewindDemo(.., true)).
    // Default: nothing (the rewind keeps its pace).
    virtual void hurryTable() {}
    // The coach's hand is still moving (gestures included).
    virtual bool bodyBusy() const = 0;

    // ---- HUD -------------------------------------------------------------------------------------
    virtual void showTakebackOffer(bool shown) = 0;    // the card with Yes / No buttons (ui::CoachHud)
    virtual void showSkipHint(bool shown) = 0;         // "Space: skip" while something skippable runs
    // Draw the glyphs of this text into the font atlas now (called at synthesis request time, so a
    // subtitle never waits for its glyphs).
    virtual void prewarmGlyphs(const std::string& written) = 0;
};

// Stockfish for the coach's analyses (the embedded engine behind ai::Engine's analysis API).
class Analyst {
public:
    virtual ~Analyst() = default;
    virtual uint32_t analyse(const ai::AnalysisRequest& request) = 0;   // 0 = no engine
    // The result once ready (true once). Calling it also pumps the engine's queue.
    virtual bool takeAnalysis(uint32_t id, ai::Analysis& out) = 0;
    virtual void stopAnalysis(uint32_t id) = 0;     // finish now with what it has (this analysis only)
    virtual void cancelAnalysis(uint32_t id) = 0;   // 0 = all of the coach's analyses
    virtual bool idle() const = 0;                  // nothing running or queued
};

}  // namespace coach
