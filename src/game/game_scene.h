// The game: main menu over the live hall, new game setup, first-person play against Stockfish
// with tournament rules (touch-move, clock pressed by hand, arbiter), animations, audio and UI.
#pragma once
#include "../ai/engine.h"
#include "../anim/animator.h"
#include "../app/scene.h"
#include "../chess/chess.h"
#include "../ui/ui.h"
#include "physical_board.h"
#include "world.h"
#include <map>
#include <string>
#include <vector>

namespace game {

class GameScene : public Scene {
public:
    bool init(AppContext& ctx) override;
    bool update(AppContext& ctx, float dt) override;
    void render(AppContext& ctx, float dt) override;
    void renderOverlay(AppContext& ctx, float dt) override;
    void shutdown(AppContext& ctx) override;

private:
    enum class State { Loading, Menu, FadeToGame, Intro, Handshake, Playing, GameOver, FadeToMenu };
    enum class Turn {
        None,
        HumanIdle,       // nothing touched yet
        HumanTouched,    // a piece is gripped on its square (touch-move applies)
        HumanPlacing,    // the hand is moving the piece (and any capture / castling rook)
        HumanPromotion,  // pawn on the last rank: choosing the new piece
        HumanPlaced,     // move made on the board, waiting for the clock press
        HumanPressing,   // hand on its way to the clock
        AiThinking,
        AiMoving
    };

    // Where a piece ends up when the hand releases it.
    struct Destination {
        chess::Square square = chess::NoSquare;  // NoSquare = off-board at 'pos'
        m::vec3 pos{0, 0, 0};
        bool captured = false;
    };

    // ---- flow ----
    void finishLoading();
    void initAnimators();
    void enterMenu();
    void setupNewGame();
    void startPlaying();
    void beginTurn();
    void endGame();
    void applySettings(bool displayToo);
    void simulate(float dt);
    void updatePlaying(float dt);
    bool isHumanTurn() const;
    void updateHumanInput();
    void offerDraw();
    void updateAi(float dt);
    void onClockPressed(int seat);
    void handleEvents(int seat, std::vector<anim::Event>& events);

    // ---- physical actions ----
    void humanTouch(int pieceId);
    void humanRelease();
    void humanPlace(chess::Square to);
    void humanPressClock();
    // Appends the tasks moving 'moverId' (already gripped) to 'to', including the capture of
    // 'victimId' (or -1) and the castling rook. Registers destinations.
    void planPlacement(std::vector<anim::Task>& tasks, int moverId, chess::Square to, int victimId,
                       chess::Square rookFrom, chess::Square rookTo);
    void planPromotionSwap(std::vector<anim::Task>& tasks, int pawnId, chess::Square sq, chess::PieceType newType);
    float carryHeight(m::vec3 from, m::vec3 to, int ignoreA, int ignoreB) const;
    m::vec3 jitteredSquare(chess::Square sq);

    // ---- helpers ----
    int seatOf(chess::Color c) const { return c == chess::White ? 0 : 1; }
    chess::Color colorOfSeat(int seat) const { return seat == 0 ? chess::White : chess::Black; }
    int humanSeat() const { return seatOf(humanColor_); }
    int aiSeat() const { return 1 - humanSeat(); }
    bool isHumanSeat(int seat) const { return !demo_ && seat == humanSeat(); }
    ai::ClockInfo clockInfo() const;
    chess::TimeControl chosenTimeControl() const;
    ai::EngineSettings chosenEngineSettings() const;
    m::Ray mouseRay() const;
    int pickPiece(const m::Ray& ray, float* tOut = nullptr) const;
    chess::Square pickSquare(const m::Ray& ray) const;
    void updateCamera(float dt, bool firstPerson);
    void placeFirstPersonCamera();
    void updateGaze(float dt);
    std::vector<Marker> markers() const;
    ClockDisplay clockDisplay() const;
    void runWarp(float seconds);
    void applyMovesInstantly(const std::vector<std::string>& uci);

    AppContext* ctx_ = nullptr;
    World world_;
    PhysicalBoard board_;
    chess::Game game_;
    chess::Arbiter arbiter_;
    chess::Clock clock_;
    ai::Engine engine_;
    bool engineOk_ = false;
    anim::Animator anim_[2];
    std::vector<anim::Event> events_;
    std::map<int, std::vector<Destination>> dest_;  // FIFO per piece (a promoted pawn moves twice)
    m::mat4 prevGlobals_[2][character::BoneCount];
    bool hasPrevGlobals_[2] = {false, false};

    State state_ = State::Loading;
    Turn turn_ = Turn::None;
    float stateTime_ = 0.0f;
    float time_ = 0.0f;
    bool paused_ = false;
    bool demo_ = false;
    bool skipIntro_ = false;
    bool warpDone_ = false;
    chess::Color humanColor_ = chess::White;
    ui::NewGameSetup setup_;
    std::string viewOverride_;

    // Touch / move state
    int touchedId_ = -1;
    chess::Square touchedSq_ = chess::NoSquare;
    chess::Square placedTo_ = chess::NoSquare;
    bool pressQueued_ = false;
    bool drawOfferPending_ = false;
    int drawOfferPly_ = -1;
    int hoverId_ = -1;

    // AI
    float aiElapsed_ = 0.0f;
    int aiThinkMs_ = 0;
    bool aiRequested_ = false;
    int lastAiEval_ = 0;
    chess::Square aiMoveTo_ = chess::NoSquare;

    // Clock
    double clockAccumMs_ = 0.0;
    float leverSide_ = -1.0f, leverTarget_ = -1.0f;

    // Camera / look
    render::Camera camera_;
    float lookYaw_ = 0.0f, lookPitch_ = 0.0f;       // user offset (radians)
    float gazeYaw_ = 0.0f, gazePitch_ = 0.0f;       // smoothed total
    float headYaw_ = 0.0f, headPitch_ = 0.0f;       // part taken by the neck/head (rest = eyes)
    float lean_ = 0.0f, leanSmooth_ = 0.0f;
    bool dragging_ = false;
    float menuAngle_ = 0.9f;
    float fade_ = 1.0f;
    float gazeTimer_ = 0.0f;
    float glanceTime_ = 0.0f;
    m::vec3 aiGazeTarget_{0, 0.8f, 0};
    m::Rng rng_{1};

    // UI
    bool showMoveList_ = false;
    bool gameOverShown_ = false;
    bool endHandshakeDone_ = false;
    float gameOverTime_ = 0.0f;
    std::string resultText_, reasonText_;
    bool playerWon_ = false, isDraw_ = false;
};

}  // namespace game
