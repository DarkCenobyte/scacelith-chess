// The game: main menu over the live hall, new game setup, first-person play against Stockfish
// with tournament rules (touch-move, clock pressed by hand, arbiter), hot-seat play (two people on
// one PC, each from their own robot's eyes, see docs/MULTIPLAYER_PLAN.md), online play (a player
// of the Scacelith server or of a direct match sits in the other chair, see
// game_scene_online.cpp), the viewer mode (two Stockfish players watched from a free, invisible
// camera), the player's Elo, animations, audio and UI.
//
// Seats: seat 0 is White's chair (+Z), seat 1 Black's (-Z). Each seat has a controller (Human,
// Stockfish or Remote; a hot-seat game has two Humans), the name and Elo written on the
// scoresheets, its engine settings and its playing hand: the hand on the clock side plays and
// presses the clock, the other one writes. The clock stands at the human's right in a game against
// Stockfish, where the New Game page puts it in a hot-seat game (the other player then plays
// left-handed), at White's right (+X) when watching.
//
// Hot-seat: the seat to move has the mouse and keyboard and the view from its robot's eyes. After
// the clock press the view flies to the other player's eyes (or cuts through black, Options >
// Gameplay) with the clock frozen; buttons still held by the previous player are ignored until
// released. Each seat keeps its own look (yaw, pitch, lean). The in-game pointer, the square
// aimed at, the clock hover, the see-through arm and the look at the scoresheet (S) belong to the
// player to move; there is no pointer or aiming while the view goes over.
//
// Command line (development and screenshots):
//   --start                 skip the menu: a game against Stockfish (--human white|black)
//   --start --hotseat       skip the menu: a hot-seat game (--white-name N --black-name N,
//                           --clock-right white|black, --rated, --handover <s> with 0 = a cut)
//   --play e2e4,e7e5,...    the human player(s) make these moves by hand, one per turn (touch,
//                           carry, clock press; the promotion piece as a fifth letter), online too
//   --viewer                skip the menu: watch Stockfish vs Stockfish (--white-preset N
//                           --black-preset N, indices into ai::presets(); --demo is an alias)
//   --viewpoint N           viewer: start at viewpoint N (0 eyes, 1 side, 2 board, 3 hall, 4 clock,
//                           5 White's face, 6 Black's face, 7 duel, 8 windows, 9 tapestries)
//   --cam x,y,z [--look x,y,z] [--fov deg]   viewer: initial observer camera; in a human game a
//                           detached camera (the player's own head is then drawn)
//   --handover-preview      viewer: follow the eyes of the player to move, flying from one to the
//                           other after each move with the clock frozen (hot-seat preview)
//   --tc N                  time control preset index for a game started from the command line
//   --no-intro --warp <s> --moves e2e4,e7e5,... --touch <square>
//   --online-mock           online play against the in-process fake server (online_mock.h)
//   --start-online [cat]    skip the menu: sign in and play the first opponent found in category
//                           "cat" (default 5+3; with --online-mock the game starts at once);
//                           --touch <square> touches that piece once the handshake is over
//   --mouse fx,fy           pointer position as fractions of the window (screenshots)
//   --glance                a human game starts looking at the player's scoresheet (S)
//   --calibrate             the brightness calibration before the title page, as on a first start
#pragma once
#include "../ai/engine.h"
#include "../anim/animator.h"
#include "../app/scene.h"
#include "../chess/chess.h"
#include "../ui/ui.h"
#include "camera_flight.h"
#include "hotseat.h"
#include "observer_camera.h"
#include "online_live.h"
#include "online_session.h"
#include "physical_board.h"
#include "scorekeeper.h"
#include "world.h"
#include <map>
#include <string>
#include <vector>

namespace game {

enum class Controller {
    Human,
    Stockfish,
    Remote   // an online opponent: its robot plays the moves the server (or direct peer) reports
};
enum class GameMode {
    Play,    // the human against Stockfish, first person
    Watch,   // viewer mode: Stockfish against Stockfish, free observer camera
    Online,  // the human against a player of the server or of a direct match, first person
    HotSeat  // two humans on this PC, in turn, each from their own robot's eyes
};

struct Seat {
    chess::Color color = chess::White;             // seat 0 White, seat 1 Black
    Controller controller = Controller::Stockfish;
    std::string name;             // scoresheet name: the player's name / "Stockfish"
    int elo = 0;                  // human: rating when the game started; Stockfish: ai::presetElo()
    bool provisional = false;     // human whose rating is provisional (elo::Record::provisional())
    std::string ratingText;       // online: the server rating as written ("1500?"), "" = none
    int preset = -1;              // Stockfish: index into ai::presets() (the last one is Custom)
    std::string presetName;       // Stockfish: "Expert", ...
    ai::EngineSettings engine;    // Stockfish: settings of this seat's searches
    character::Side playHand = character::Side::Right;  // plays and presses the clock
    bool human() const { return controller == Controller::Human; }
    character::Side writingHand() const {
        return playHand == character::Side::Right ? character::Side::Left : character::Side::Right;
    }
};

class GameScene : public Scene {
public:
    bool init(AppContext& ctx) override;
    bool update(AppContext& ctx, float dt) override;
    void render(AppContext& ctx, float dt) override;
    void renderOverlay(AppContext& ctx, float dt) override;
    void shutdown(AppContext& ctx) override;

    // ---- For the scoresheets and the hot-seat mode ----
    const Seat& seat(int index) const { return seats_[index & 1]; }
    GameMode mode() const { return mode_; }
    int round() const { return round_; }   // games started this session (scoresheet "Round")
    // Clock freeze (hot-seat handover): while frozen the running clock does not count (neither its
    // time nor its delay window) and the next player does not act yet. See docs/MULTIPLAYER_PLAN.md.
    void setClockFrozen(bool frozen) { clockFrozen_ = frozen; }
    bool clockFrozen() const { return clockFrozen_; }

private:
    static constexpr float kBaseGazePitch = -0.62f;  // looking down at the board from the chair
    static constexpr float kHeadYawLimit = 70.0f * m::DEG;
    static constexpr float kHeadPitchDown = -45.0f * m::DEG, kHeadPitchUp = 30.0f * m::DEG;
    // Hot-seat: after the landing, a piece touched within this time defers the player's recording
    // of the opponent's move until after their own move (FIDE 8.1.2).
    static constexpr float kWriteGrace = 0.5f;
    // --play: the scripted player looks at the position this long before each move.
    static constexpr float kScriptThink = 0.5f;

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
        AiMoving,
        RemoteWaiting,   // online: waiting for the opponent's move
        RemoteMoving     // online: the opponent's robot is placing the move
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
    void configureSeats();
    void startPlaying();
    void beginTurn();
    void endGame();
    void rateGame();          // human games: Elo update once the result is known (idempotent)
    ui::GameOverExtras gameOverExtras() const;
    void applySettings(bool displayToo);
    void simulate(float dt);
    void updatePlaying(float dt);
    bool isHumanTurn() const;
    void updateHumanInput();
    void offerDraw();
    void updateAi(float dt);
    void onClockPressed(int seat);
    void answerAiDrawOffer(int offeringSeat);
    void handleEvents(int seat, std::vector<anim::Event>& events);

    // ---- physical actions ----
    void humanTouch(int pieceId);
    void humanRelease();
    void humanPlace(chess::Square to);
    void humanPressClock();
    // Appends the tasks moving 'moverId' (already gripped) to 'to', including the capture of
    // 'victimId' (or -1) and the castling rook. Registers destinations. 'lifted': the piece is
    // already in the air (an online opponent's piece held live), no Lift.
    void planPlacement(std::vector<anim::Task>& tasks, int moverId, chess::Square to, int victimId,
                       chess::Square rookFrom, chess::Square rookTo, bool lifted = false);
    void planPromotionSwap(std::vector<anim::Task>& tasks, int pawnId, chess::Square sq, chess::PieceType newType);
    float carryHeight(m::vec3 from, m::vec3 to, int ignoreA, int ignoreB) const;
    m::vec3 jitteredSquare(chess::Square sq);

    // ---- helpers ----
    bool pieceInHand(const PieceObject& p) const;
    int seatOf(chess::Color c) const { return c == chess::White ? 0 : 1; }
    chess::Color colorOfSeat(int seat) const { return seat == 0 ? chess::White : chess::Black; }
    int humanSeat() const { return seatOf(humanColor_); }
    int aiSeat() const { return 1 - humanSeat(); }
    bool isHumanSeat(int seat) const { return seats_[seat & 1].human(); }
    bool watching() const { return mode_ == GameMode::Watch; }
    bool online() const { return mode_ == GameMode::Online; }
    bool hotSeat() const { return mode_ == GameMode::HotSeat; }
    // The seat whose player has the mouse and keyboard: the human, or in a hot-seat game the seat
    // to move (hotseat::inputSeat).
    int inputSeat() const;
    chess::Color inputColor() const { return colorOfSeat(inputSeat()); }
    // The seat whose eyes hold the first-person camera (-1 during a hot-seat flight) and the seat
    // whose head follows the first-person look (during a flight: the next player's).
    int viewSeat() const;
    int firstPersonSeat() const;
    bool opponentMoving() const { return turn_ == Turn::AiMoving || turn_ == Turn::RemoteMoving; }
    // The player's robot presses the clock by itself once the move is on the board: online the
    // authority decides (og_.autoPress), on this PC Options > Gameplay (off by default).
    bool autoPressClock() const;
    ai::ClockInfo clockInfo() const;
    chess::TimeControl chosenTimeControl() const;
    ai::EngineSettings chosenEngineSettings() const { return engineSettingsFor(setup_.difficulty); }
    ai::EngineSettings engineSettingsFor(int preset) const;
    m::vec2 cursorPixels() const;             // the pointer (physical pixels), or --mouse
    m::Ray mouseRay() const;
    int pickPiece(const m::Ray& ray, float* tOut = nullptr) const;
    chess::Square pickSquare(const m::Ray& ray) const;
    // The square a click designates while a piece is in hand (NoSquare: none). 'castling' is set
    // when it is the king's castling square designated by pointing at the rook.
    chess::Square aimSquare(const m::Ray& ray, bool* castling = nullptr) const;
    bool legalDestination(chess::Square to) const;  // for the touched piece
    bool gameCursorShown() const;             // the game's pointer replaces the system arrow
    ui::GameCursor gameCursorKind() const;
    m::vec3 glanceTarget() const;             // where the player looks at their scoresheet (S)
    m::vec3 glanceTarget(int seat) const;     // the middle of that seat's scoresheet
    void updateCamera(float dt, bool firstPerson);
    void placeFirstPersonCamera();
    // The first-person view from 'seat''s eyes with its own look (gaze beyond the head, lean).
    void firstPersonView(int seat, m::vec3& position, m::quat& orientation) const;
    CameraPose firstPersonPose(int seat) const;
    void updateGaze(float dt);
    std::vector<Marker> markers() const;
    ClockDisplay clockDisplay() const;
    void runWarp(float seconds);
    void applyMovesInstantly(const std::vector<std::string>& uci);
    // ---- scoresheets ----
    void newScoresheets();                     // blank pads for the game just set up
    int handStyleOf(int seat) const;           // the seat's handwriting (ui::font::HandStyle)

    // ---- hot-seat (two players on one PC) ----
    void initHotSeatArgs();                    // command line: --hotseat and its options
    void configureHotSeatSeats();
    void startHandover(int mover);             // after a clock press: the view goes to the other player
    void updateHandover(float dt);             // inside simulate, after the animation update
    void landHandover();
    void beginLook(int seat, bool snap);       // the seat's first-person look takes its head over
    void updateHotSeatTurn(float dt);          // writing grace, draw offer card, scripted moves (Playing)
    void drawHotSeatHud();                     // players, caption, draw offer card (answers it)
    void offerDrawHotSeat();
    void answerHotSeatDraw(bool accept);
    void rateHotSeat();                        // rated games: both local ratings (idempotent via rated_)
    ui::GameOverExtras hotSeatGameOverExtras() const;
    void swapHotSeatColours();                 // rematch
    bool anyInputHeld() const;
    void updateScript(float dt);               // --play: the next scripted move, when idle

    // ---- online play (game_scene_online.cpp) ----
    struct RemoteMove { int ply = 0; uint16_t move = 0; };
    void initOnline();                        // command line: --online-mock, --start-online
    bool takeOnlineGame();                    // a game announced by the session: set up and play it
    void setupOnlineGame();                   // part of setupNewGame() for an online game
    void configureOnlineSeats();
    Scorekeeper::Details onlineSheetDetails() const;
    void updateOnline(float dt);              // once per frame (inside simulate): events, remote moves
    void onlineEvent(const net::Event& e);
    void onlineSnapshot(const net::OnlineGame& g);
    void onlineGameEvent(const net::Event& e);
    void rebuildOnline();                     // board, game and sheets from og_ (no animation)
    void startRemoteMove();
    // The opponent's move 'mv' from the piece in hand to the board (placement, capture, castling
    // rook, promotion swap), without the clock press; 'lifted': the piece is already in the air.
    void planRemoteMove(std::vector<anim::Task>& tasks, const chess::Move& mv, bool lifted);
    // My move chosen: sent at once when the robots press the clock by themselves, otherwise staged
    // on the board until my clock press sends it (pressOnlineClock, at the lever contact).
    void placeOnlineMove(const chess::Move& mv);
    void pressOnlineClock();
    void dropStagedMove();                    // the game ended meanwhile: the staged move never goes
    void sendOnlineMove(const chess::Move& mv);
    double localMs() const;                   // steady local clock (thinking time, clock display)
    // My hand and head for the opponent's robot (net/gesture.h), once per frame.
    live::Hand onlineHand(float dt);
    void sendOnlineGesture(float dt);
    // The opponent's robot mirrors their gestures: the piece in hand, where it is aimed and the move
    // put down before their clock press (updateRemoteLive, once per frame while playing), their
    // head and lean (driveRemoteHead, from updateGaze: true while it drives the head).
    void updateRemoteLive(float dt);
    void enqueueRemoteLive(const std::vector<anim::Task>& tasks);  // on their robot, as live work
    void gripRemoteLive(chess::Square from, int ply);
    void followRemoteAim(int aim, float dt);
    void placeRemoteLive(uint16_t move);
    // Lets go of the piece held live: back on its square (then the hand retracts, unless another
    // task follows). A move put down is taken back instead (settleRemoteTakeBack).
    void cancelRemoteLive(bool retract = true);
    bool remoteMoveQueued(int ply) const;     // the opponent's MoveMade of that ply waits for the robot
    void settleRemoteTakeBack();
    bool driveRemoteHead(float dt);
    void recordOnline(int ply);               // scoresheets: every move up to 'ply'
    void onlineResult();                      // result texts of og_ (endGame)
    void updateOnlineInput();                 // Esc menu, draw offer, report dialog (Playing)
    void updateOnlineGameOver();              // game over card, rematch, report, challenges
    void leaveOnlineGame();                   // back to the menu
    void drawOnlineHud();                     // ping, banners, first-move countdown
    ClockDisplay onlineClockDisplay() const;
    ui::GameOverExtras onlineGameOverExtras() const;
    int64_t onlineClockMs(int color) const;   // server time, extrapolated
    bool myFirstMoveMade() const;

    // ---- viewer mode ----
    bool observerView() const;                // the observer camera is the view
    void updateWatchInput();                  // once per frame: menu, viewpoints, overlay
    void updateObserver(float dt);            // the observer camera (inside simulate)
    CameraPose viewpoint(int n) const;        // presets 1..9 (0 = eyes of the player to move)
    CameraPose eyePose(int seat) const;
    void selectViewpoint(int n, bool jump);
    int headNearCamera(m::vec3 p) const;      // seat whose head contains p (drawn headless), or -1
    float observerFocus(const render::Camera& cam) const;
    float firstPersonFocus(const m::Ray& gaze) const;  // distance the player's eyes focus at

    AppContext* ctx_ = nullptr;
    GameMode mode_ = GameMode::Play;
    Seat seats_[2];
    int round_ = 0;
    World world_;
    PhysicalBoard board_;
    chess::Game game_;
    chess::Arbiter arbiter_;
    chess::Clock clock_;
    ai::Engine engine_;
    bool engineOk_ = false;
    anim::Animator anim_[2];
    Scorekeeper scorekeeper_;                  // both scoresheets and the writing hands' work
    std::vector<anim::Event> events_;
    std::map<int, std::vector<Destination>> dest_;  // FIFO per piece (a promoted pawn moves twice)
    m::mat4 prevGlobals_[2][character::BoneCount];
    bool hasPrevGlobals_[2] = {false, false};

    State state_ = State::Loading;
    Turn turn_ = Turn::None;
    float stateTime_ = 0.0f;
    float time_ = 0.0f;
    bool paused_ = false;
    bool startWatching_ = false;  // --viewer
    bool skipIntro_ = false;
    bool warpDone_ = false;
    chess::Color humanColor_ = chess::White;
    ui::NewGameSetup setup_;
    ui::WatchSetup watch_;
    bool debugCamera_ = false;    // --cam in a human game

    // Touch / move state
    int touchedId_ = -1;
    chess::Square touchedSq_ = chess::NoSquare;
    chess::Square placedTo_ = chess::NoSquare;
    bool pressQueued_ = false;
    bool drawOfferPending_ = false;
    int drawOfferPly_ = -1;
    int hoverId_ = -1;
    chess::Square aimSq_ = chess::NoSquare;   // square under the pointer while a piece is in hand
    bool aimLegal_ = false;                   // aimSq_ is a legal destination
    bool clockHover_ = false;                 // the pointer is on the clock
    bool pressTouched_ = false;               // this left press touched a piece: releasing it on
    m::vec2 pressPos_{0, 0};                  // another square moves the piece there (drag)
    bool osCursorHidden_ = false;
    bool mouseOverride_ = false;              // --mouse
    m::vec2 mouseOverridePos_{0.5f, 0.5f};

    // AI
    float aiElapsed_ = 0.0f;
    int aiThinkMs_ = 0;
    bool aiRequested_ = false;
    int lastAiEval_ = 0;
    int lastEval_[2] = {0, 0};          // each AI's last search score (its own point of view)
    bool hasEval_[2] = {false, false};
    int lastOfferPly_[2] = {-1, -1};    // AI vs AI draw offers
    int pendingOffer_ = -1;             // seat whose offer comes with the move being played
    bool aiHasMove_ = false;
    chess::Move aiMove_;
    chess::Square aiMoveTo_ = chess::NoSquare;

    // Clock
    bool clockFrozen_ = false;
    double clockAccumMs_ = 0.0;
    float leverSide_ = -1.0f, leverTarget_ = -1.0f;

    // Camera / look: one first-person look per seat (hot-seat: each player keeps theirs)
    render::Camera camera_;
    struct Look {
        float yaw = 0.0f, pitch = 0.0f;             // user offset (radians)
        float gazeYaw = 0.0f, gazePitch = kBaseGazePitch;  // smoothed total
        float headYaw = 0.0f, headPitch = 0.0f;     // part taken by the neck/head (rest = eyes)
        float lean = 0.0f, leanSmooth = 0.0f;
        // Pointer at the top of the window (look_up.h): the band lifts the gaze once the pointer
        // has been below it since the last reset, and the lift goes into pitch when a drag starts.
        bool lookUpArmed = false;
        float lookUpLift = 0.0f;
    };
    Look look_[2];
    // S: the player whose eyes are the view looks at their own scoresheet. One state for the
    // view, not per seat: in a hot-seat game a turn's look ends at the clock press (startHandover).
    bool glance_ = false;
    float glanceBlend_ = 0.0f;
    bool dragging_ = false;
    float menuAngle_ = 0.9f;
    float fade_ = 1.0f;
    float focusDistance_ = -1.0f;
    bool cameraCut_ = true;
    float gazeTimer_ = 0.0f;
    float glanceTime_ = 0.0f;
    m::vec3 aiGazeTarget_{0, 0.8f, 0};
    m::Rng rng_{1};
    float armSeeThrough_ = 0.0f;   // player's playing arm: 0 opaque .. 1 see-through (piece in hand)
    int armSeeThroughSeat_ = 0;    // whose arm it is (the player who carried the piece)

    // Viewer mode
    ObserverCamera observer_;
    ObserverCamera::Controls observerControls_;
    bool followEyes_ = false;           // viewpoint 0: through the eyes of the player to move
    int eyesSeat_ = -1;
    CameraPose eyesSmooth_;             // steadied eye pose (the robots' saccades)
    bool handoverPreview_ = false;      // --handover-preview
    int pendingViewpoint_ = -1;         // jump once the robots are posed
    bool pendingCamArg_ = false;        // --cam: initial observer camera
    bool observerPlaced_ = false;       // since the last visit to the menu
    int viewpointShown_ = -1;           // overlay label
    float viewpointAge_ = 1e9f, speedAge_ = 1e9f;
    bool hudVisible_ = true;

    // Hot-seat
    hotseat::Players hsPlayers_;        // the two players of the game being played
    hotseat::Handover handover_;
    hotseat::InputGate inputGate_;      // buttons held by the previous player
    bool inputBlocked_ = false;         // this frame: the gate holds the input back
    int viewSeat_ = 0;                  // the seat whose eyes the view is in (outside a handover)
    float writeGrace_ = 0.0f;           // after the landing: a piece touched before it ends defers the writing
    int drawOfferBy_ = -1;              // seat whose draw offer goes with its next clock press
    int drawCardFor_ = -1;              // seat asked to accept a draw (card)
    float captionAge_ = 1e9f;           // "Bob, your move" (since the handover began)
    float handoverArg_ = -1.0f;         // --handover <s> (this session only); -1 = Options > Gameplay
    float scriptWait_ = 0.0f;           // --play: a short pause before each scripted move
    int hsEloBefore_[2] = {0, 0}, hsEloAfter_[2] = {0, 0};
    // --play: moves made by hand by the human player(s), one per turn
    std::vector<std::string> script_;
    size_t scriptPos_ = 0;
    chess::PieceType scriptPromo_ = chess::NoPiece;

    // UI
    bool showMoveList_ = false;
    bool gameOverShown_ = false;
    bool endHandshakeDone_ = false;
    float gameOverTime_ = 0.0f;
    std::string resultText_, reasonText_;
    bool playerWon_ = false, isDraw_ = false;
    // Elo of the game just played (human games)
    bool rated_ = false;                // rateGame() ran for this game
    bool eloCounted_ = false;           // the game changed the rating
    int eloBefore_ = 0, eloAfter_ = 0;

    // Online game
    GameLink* link_ = nullptr;          // owned by onlineSession()
    net::OnlineGame og_;                // authoritative state as last reported
    std::vector<RemoteMove> remoteQueue_;  // opponent moves waiting for the robot
    int pendingPly_ = -1;               // my move sent, not confirmed yet
    uint16_t pendingMove_ = 0;
    int recordedPly_ = 0;               // moves handed to the scoresheets
    int pressedPly_ = -1;               // my last move whose clock press was animated
    int remotePly_ = -1;                // the move the opponent's robot is playing
    float endWait_ = 0.0f;              // time the end has waited for the robots
    chess::Square promoTo_ = chess::NoSquare;  // pawn move waiting for the promotion choice
    double turnStartMs_ = 0;            // localMs() when my turn began (thinkMs)
    // My move on its way, kept as sent (sent again when the connection dropped meanwhile).
    std::string pendingFen_;            // the position before it (the authority checks its digest)
    uint32_t pendingThinkMs_ = 0;
    live::ClockFreeze clockFreeze_;     // my clock display while my move is on its way
    bool virtualTime_ = false;          // screenshots, --warp: localMs() follows the simulated time
    // Manual clock press (og_.autoPress off): my move stands on the board until my press.
    bool moveStaged_ = false;
    chess::Move stagedMove_;
    // My gestures (sendOnlineGesture): the last one built and the last one sent.
    net::Gesture gestureBuilt_, gestureSent_;
    bool gestureBuiltAny_ = false, gestureSentAny_ = false;
    bool gestureFinal_ = false;         // the game is over and its last gesture went
    double gestureSinceMs_ = 0;         // since the last send
    live::Dwell aimDwell_{live::kAimDwell};
    chess::Square aimDwellFor_ = chess::NoSquare;  // the touched square the dwell belongs to
    // The opponent's gestures and what their robot's hand does with them.
    struct RemoteLive {
        int pieceId = -1;                          // their piece held live (touched, lifted, carried)
        chess::Square from = chess::NoSquare;      // its square
        chess::Square hover = chess::NoSquare;     // the square the hand was last sent over
        int ply = -1;                              // the ply of the move being prepared
        uint16_t placed = 0;                       // the move put down on the board, before its MoveMade
        bool takeBack = false;                     // put the board back from game_ once the hands are idle
        float noAim = 0.0f;                        // time without an aim (then back over its square)
        float placedAway = 0.0f;                   // time the gestures no longer show 'placed'
        float reachAt = 0.0f;                      // animator clock: the hand starts reaching for it
    };
    RemoteLive remoteLive_;
    float remoteLiveEnd_ = 0.0f;        // animator clock: their robot is done with its live tasks
    net::Gesture remoteGesture_;        // the latest one
    float remoteAge_ = 1e9f;            // seconds since it arrived
    bool remoteFresh_ = false;          // it came after the last snapshot and our last reconnection
    live::Dwell remoteAim_{live::kFollowDwell};
    live::HeadSpring remoteHead_;
    bool remoteHeadOn_ = false;         // the opponent's head drives their robot's (head override)
    bool remoteGlancing_ = false;       // they look at their scoresheet: their writing hand waits aside
    float remoteGlanceBlend_ = 0.0f;
    bool resync_ = false;               // rebuild once the robots are idle
    bool rebuildFade_ = false;
    bool endPending_ = false;           // GameEnd received, shown once the moves are played
    bool drawOffered_ = false;          // the opponent offers a draw (card)
    bool myDrawOffer_ = false;          // my offer is pending
    bool opponentAway_ = false;
    double opponentBackBy_ = 0;         // server clock: end of the opponent's grace period
    bool ratingKnown_ = false;
    int ratingBefore_ = 0, ratingAfter_ = 0;
    bool rematchAsked_ = false, rematchOffered_ = false, rematchGone_ = false;
    bool reportOpen_ = false, reported_ = false;
    int reportCategory_ = 0;
    std::string reportComment_;
    bool onlinePauseLeave_ = false;
    std::string startOnline_;           // --start-online category
    std::string startTouch_;            // --start-online with --touch <square>: touched once idle
    float fadeDip_ = 0.0f;              // short darkening while the board is rebuilt
};

}  // namespace game
