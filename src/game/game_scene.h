// The game: main menu over the live hall, new game setup, first-person play against Stockfish
// with tournament rules (touch-move, clock pressed by hand, arbiter), hot-seat play (two people on
// one PC, each from their own robot's eyes, see docs/MULTIPLAYER_PLAN.md), online play (a player
// of the Scacelith server or of a direct match sits in the other chair, see
// game_scene_online.cpp), the viewer mode (two Stockfish players watched from a free, invisible
// camera), the replay of a saved game (game_scene_replay.cpp), the Analysis mode
// (game_scene_analysis.cpp), the player's Elo, animations, audio and UI.
//
// Seats: seat 0 is White's chair (+Z), seat 1 Black's (-Z). Each seat has a controller (Human,
// Stockfish or Remote; a hot-seat game has two Humans), the name and Elo written on the
// scoresheets, its engine settings and its playing hand: the hand on the clock side plays and
// presses the clock, the other one writes. The clock stands at the human's right in a game against
// Stockfish, where the New Game page puts it in a hot-seat game (the other player then plays
// left-handed), at White's right (+X) when watching.
//
// Hot-seat: the seat to move has the mouse and keyboard and the view from its robot's eyes. After
// the clock press (untimed: once the move is made) the view flies to the other player's eyes (or
// cuts through black, Options > Gameplay) with the clock frozen; buttons still held by the
// previous player are ignored until released. Each seat keeps its own look (yaw, pitch, lean).
// The in-game pointer, the square aimed at, the clock hover, the see-through arm and the look at
// the scoresheet (S) belong to the player to move; there is no pointer or aiming while the view
// goes over.
//
// Untimed games (no time control; Play, hot-seat and Watch can be, Coach always is, a replay is
// when its record has no TimeControl or clocks, online never is, see clock_rules.h) have no clock
// press: a move is completed as its last piece is released and the turn passes at once
// (completeMove). The clock shows dashes, its lever stays still.
//
// Coach mode (game_scene_coach.cpp): the human against the coach robot ("COACH" on its chest),
// levels 1-6, or the rules lesson (level 0). Untimed, never rated, touch-move on, illegal
// placements refused. The scene is the coach's coach::Stage (its voice, subtitles, gestures, gaze,
// demonstration moves, takebacks, marks, HUD) and coach::Analyst (the Stockfish analyses); a
// coach::Session decides what the coach says and when (src/coach). The coach's own moves come
// from its teaching repertoire, then Stockfish at the level's strength, and wait while the
// session reviews the player's move (coachMayMove).
//
// Saved games (game_archive.h, game_saving.h): games against Stockfish, coach games of levels 1-6,
// hot-seat games and direct matches go to plat::appDataDirectory() + "pgn/" as PGN files, with the
// time each move took and the clocks, when they end or are left (archiveGame); never server games,
// the viewer mode, the rules lesson or a replay. The title page's "Saved games" lists them.
//
// Replay (game_scene_replay.cpp): a saved game played again by the two robots at the pace it was
// played (replay::ReplayClock), watched like the viewer mode (free camera, viewpoints, the
// players' eyes), its clocks from the record, its scoresheets written as the moves are played,
// its result card at the end. K pauses, J / L step back / forward, Shift+J / Shift+L slower /
// faster, Home / End the start / the end; the same as mouse buttons on the overlay.
//
// Analysis (game_scene_analysis.cpp): a game reviewed by Stockfish (src/analysis), opened from
// the Analysis page, the Saved games page, the online history or the game over card of a game just
// played. A replay the player steps through: one move forward played by a robot, one move back
// taken back by hand, further set at once behind a dip; the evaluation bar and the move list
// (ui::analysisHud), the symbols and better-move arrows on the board (World::submitAnalysisMarks),
// the commentator's comments on the key moments (voice and subtitles). K plays / pauses, J / L one
// move back / forward, Home / End, N the comments, M the voice, B the arrows.
//
// Standing up (anim/stance.h, the rules in stance_control.h): during a game the first-person
// player gets up with the arrow keys (Up: in front of the chair, Left / Right: at that end of the
// table, Down: back in the chair). Only a seated player plays: standing, nothing on the board or
// the clock answers (a notice says to sit down), the scoresheet waits, the clock keeps running.
// The view rides the robot's eyes and aims at the board from wherever it stands. Online the
// opponent sees the robot get up (GameLink::sendStance) and their robot does the same. At the end
// of a game everyone sits back down before the handshake.
//
// Command line (development and screenshots):
//   --start                 skip the menu: a game against Stockfish (--human white|black)
//   --start --coach         skip the menu: a coach game (--coach-level N, 0 = the rules lesson,
//                           1..6; --coach-colour white|black, or --coach-color); the [coach]
//                           settings otherwise
//   --coach-dir <path>      the folder of the coach's voice files
//                           (default: <application data>/coach/, see tts/model_store.h)
//   --coach-stage-test      a coach game whose Stage performs a fixed sequence (a spoken line with
//                           its subtitle, pointing, a knight's trace, marks, two demonstration
//                           moves and their rewind, the takeback card), without the session;
//                           --coach-stage-test lesson: on the rules lesson's first position
//   --coach-auto-answer yes|no   the takeback card answers itself after 1.5 s (with --play)
//   --analysis-marks-test   once the pieces stand, the review's marks (a fixed set: tint, badges,
//                           arrows; World::submitAnalysisMarks) arrive on the board
//   --start --hotseat       skip the menu: a hot-seat game (--white-name N --black-name N,
//                           --clock-right white|black, --rated, --handover <s> with 0 = a cut)
//   --play e2e4,e7e5,...    the human player(s) make these moves by hand, one per turn (touch,
//                           carry, clock press unless untimed; the promotion piece as a fifth
//                           letter), online too
//   --viewer                skip the menu: watch Stockfish vs Stockfish (--white-preset N
//                           --black-preset N, indices into ai::presets(); --demo is an alias)
//   --viewpoint N           viewer: start at viewpoint N (0 eyes, 1 side, 2 board, 3 hall, 4 clock,
//                           5 White's face, 6 Black's face, 7 duel, 8 windows, 9 tapestries)
//   --cam x,y,z [--look x,y,z] [--fov deg]   viewer: initial observer camera; in a human game a
//                           detached camera (the player's own head is then drawn)
//   --handover-preview      viewer: follow the eyes of the player to move, flying from one to the
//                           other after each move with the clock frozen (hot-seat preview)
//   --tc N                  time control preset index for a game started from the command line
//                           (0 = no clock: an untimed game)
//   --no-intro --warp <s> (with --shot) --moves e2e4,e7e5,... --touch <square>
//   --online-mock           online play against the in-process fake server (online_mock.h)
//   --start-online [cat]    skip the menu: sign in and play the first opponent found in category
//                           "cat" (default 5+3; with --online-mock the game starts at once);
//                           --touch <square> touches that piece once the handshake is over
//   --start-online direct   with --online-mock: a direct match against the fakes' friend
//   --mock-stance standing|side-left|side-right   with --online-mock: the fake opponent stands
//                           there (net::mock::forceOpponentStance; F11 in a mock game cycles it:
//                           its own outings, standing, left, right, seated)
//   --play-then a,b,...     once the --play moves are made, the player picks these in the Esc menu,
//                           one per turn: resign, leave (Main menu), takeback (coach games)
//   --replay <file.pgn>     skip the menu: replay a saved game (--game N: the Nth game of the file,
//                           from 1; --replay-speed x1|x2|x4|x8|instant; --replay-paused;
//                           --replay-keys K,L,J,Shift+L,Home,End,... presses these keys in turn,
//                           each once the board is still; Leave: Esc and "Main menu")
//   --analyse <file.pgn>    skip the menu: analyse a game (--game N; --analysis-at N opens it at
//                           position N; --replay-keys J,L,K,Home,End,N,M,B,Goto:N,Wait:S,Leave)
//   --mouse fx,fy           pointer position as fractions of the window (screenshots)
//   --glance                a human game starts looking at the player's scoresheet (S)
//   --stance standing|side-left|side-right   the local first-person player takes that stance
//                           once the game is Playing and the --play moves are made (screenshots)
//   --calibrate             the brightness calibration before the title page, as on a first start
#pragma once
#include "../ai/engine.h"
#include "../anim/animator.h"
#include "../app/scene.h"
#include "../chess/chess.h"
#include "../ui/ui.h"
#include "camera_flight.h"
#include "clock_rules.h"
#include "game_archive.h"
#include "coach_args.h"
#include "game_mode.h"
#include "hotseat.h"
#include "observer_camera.h"
#include "online_live.h"
#include "online_session.h"
#include "physical_board.h"
#include "replay.h"
#include "scorekeeper.h"
#include "stance_control.h"
#include "turn.h"
#include "world.h"
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace coach {
class Stage;
}

namespace game {

struct CoachRuntime;   // game_scene_coach.cpp
class CoachStage;
class CoachAnalyst;
struct AnalysisRuntime;   // game_scene_analysis.cpp
class AnalysisStage;

enum class Controller {
    Human,
    Stockfish,
    Remote   // an online opponent: its robot plays the moves the server (or direct peer) reports
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
    GameScene();
    ~GameScene() override;
    bool init(AppContext& ctx) override;
    bool update(AppContext& ctx, float dt) override;
    void render(AppContext& ctx, float dt) override;
    void renderOverlay(AppContext& ctx, float dt) override;
    void shutdown(AppContext& ctx) override;

    // ---- For the hot-seat mode ----
    // Clock freeze (hot-seat handover): while frozen the running clock does not count (neither its
    // time nor its delay window) and the next player does not act yet. See docs/MULTIPLAYER_PLAN.md.
    void setClockFrozen(bool frozen) { clockFrozen_ = frozen; }

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
    using Turn = game::Turn;   // turn.h

    // Where a piece ends up when the hand releases it.
    struct Destination {
        chess::Square square = chess::NoSquare;  // NoSquare = off-board at 'pos'
        m::vec3 pos{0, 0, 0};
        bool captured = false;
        bool reserve = false;                    // a spare back in the reserve (a promotion taken back)
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
    // No move on its way between the board and the clock: a draw agreed now leaves on the board
    // what game_ has.
    bool quietTurn() const { return game::quietTurn(turn_); }
    void updateHumanInput();
    void offerDraw();
    void updateAi(float dt);
    // The game ends or is left while Stockfish searches the AI's move: the search is cancelled (it
    // would go on to the end of its time budget). Not in coach games (their closing analyses).
    void cancelAiSearch();
    void onClockPressed(int seat);
    // The move on the board is completed (FIDE 6.2.1; untimed: 4.7): the arbiter's verdict, the
    // move into game_, the scoresheets, the end of the game, then the turn passes (hot-seat: the
    // handover). The one entry point for every completed offline move: the clock press
    // (onClockPressed) of a timed game, the last piece released (updatePlaying) of an untimed one.
    // Ignored unless a move by 'seat' waits for its completion.
    void completeMove(int seat);
    void answerAiDrawOffer(int offeringSeat);
    void handleEvents(int seat, std::vector<anim::Event>& events);

    // ---- standing up (stance_control.h) ----
    // The seat's player may play: its robot is in the chair and not asked to get up.
    bool seatMayPlay(int seat) const;
    // The seat's stance target (anim::Animator::setStance), its scoresheet's hold at once (a move
    // recorded later in the same frame waits for the robot to sit down).
    void setSeatStance(int seat, anim::Stance target);
    // The seat's writing waits while it may not play or hot-seat holds it (Scorekeeper::setHold).
    void syncWritingHold(int seat);
    void setHotSeatHold(int seat, bool hold);
    // The arrow keys of the first-person player, and --stance (Playing, once per frame).
    void updateStanceInput();
    void noticeSitToPlay();                    // the player tried to play standing (rate-limited)

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
    // The move 'mv' of 'side' from the piece in hand to the board (placement, capture, castling
    // rook, promotion swap), without the clock press; 'lifted' as for planPlacement.
    void planMove(std::vector<anim::Task>& tasks, const chess::Move& mv, chess::Color side, bool lifted = false);
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
    // The viewer mode, a replay and an analysis: all are watched from the free observer camera,
    // with the viewpoints, the overlay and the Esc menu of the viewer (replaying() tells them
    // apart). An analysis is a replay (the record's game, its players, no clock running) that the
    // player steps through (analysing() tells it apart).
    bool watching() const { return mode_ == GameMode::Watch || replaying(); }
    bool replaying() const { return mode_ == GameMode::Replay || mode_ == GameMode::Analysis; }
    bool analysing() const { return mode_ == GameMode::Analysis; }
    bool online() const { return mode_ == GameMode::Online; }
    bool hotSeat() const { return mode_ == GameMode::HotSeat; }
    // No time control: no clock press, the move is completed as its last piece is released
    // (clock_rules.h). Never online.
    bool untimed() const { return untimedGame(online(), clock_.timeControl()); }
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
    // authority decides (og_.autoPress), on this PC Options > Gameplay (off by default). Untimed
    // games have no press at all (untimed()).
    bool autoPressClock() const;
    ai::ClockInfo clockInfo() const;
    chess::TimeControl chosenTimeControl() const;
    ai::EngineSettings engineSettingsFor(int preset) const;
    m::vec2 cursorPixels() const;             // the pointer (physical pixels), or --mouse
    m::Ray mouseRay() const;
    // The piece under the pointer: the first whose outline the ray meets (rayHitsPiece).
    int pickPiece(const m::Ray& ray, float* tOut = nullptr) const;
    // Distance along the ray to the outline of a piece standing at its resting place, grown by
    // 'margin' (World::pieceSilhouette), searched up to maxT; -1 when it misses. Before the chess
    // set is built: a cylinder around it.
    float rayHitsPiece(const PieceObject& p, const m::Ray& ray, float margin = 0.0f, float maxT = 1e30f) const;
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
    void startHandover(int mover);             // move completed (completeMove): the view goes to the other player
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
    void updateScript(float dt);               // --play: the next scripted move, when idle; then --play-then
    // The action of the Esc menu chosen this frame: the one --play-then queued (scripted runs),
    // else what 'shown' (the menu drawn this frame) returned.
    ui::MenuAction menuChoice(ui::MenuAction shown);

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
    // Stances (protocol minor 2): mine to the authority, the opponent's on their robot (once per
    // frame, inside updateOnline); 'seated' sets theirs to Seated at once (their move, the end).
    void updateStances(float dt);
    void opponentSeated();
    void recordOnline(int ply);               // scoresheets: every move up to 'ply'
    void onlineResult();                      // result texts of og_ (endGame)
    void updateOnlineInput();                 // Esc menu, draw offer, report dialog (Playing)
    void updateOnlineGameOver();              // game over card, rematch, report, challenges
    bool updateReportDialog();                // the report dialog, while it is open (true)
    void sendReport();                        // the report filled in, to the server (ReportResult)
    void leaveOnlineGame();                   // back to the menu
    void leaveOngoingOnlineGame();            // aborts before my first move, else resigns
    void drawOnlineHud();                     // ping, banners, first-move countdown
    ClockDisplay onlineClockDisplay() const;
    ui::GameOverExtras onlineGameOverExtras() const;
    int64_t onlineClockMs(int color) const;   // server time, extrapolated
    bool myFirstMoveMade() const;

    // ---- Coach mode (game_scene_coach.cpp) ----
    friend class CoachStage;
    friend class CoachAnalyst;
    bool coach() const { return mode_ == GameMode::Coach; }
    bool lesson() const { return coach() && coachLevel_ == 0; }   // the rules lesson
    // Legal-move hints: the option, forced on in the rules lesson.
    bool legalHints() const;
    void initCoachArgs();                     // command line: --coach and its options
    void refreshCoachVoice();                 // are the voice's model files there (tts::modelFilesPresent)
    void coachModelDownloaded(int fetched);   // a download ended: a failed voice may get one more try
    CoachRuntime& coachRuntime();             // created on first use: the voice starts loading
    // The coach's TTS worker, as a stage's voice requests (requestSpeech, takeSpeech, speechFailed,
    // cancelSpeech, voiceAvailable): shared with the Analysis mode's commentator, so that one model
    // is ever loaded. ensureCoachVoiceWorker() starts it when the voice files are there.
    coach::Stage& coachVoice();
    void ensureCoachVoiceWorker();
    bool coachVoiceExpected() const;          // the voice files are there (the coach page's notice)
    void setupCoachGame();                    // part of setupNewGame() for a coach game
    void configureCoachSeats();
    void startCoachGame();                    // startPlaying(): the session begins
    void leaveCoachGame();                    // back to the menu, a new game, shutdown: silence
    void shutdownCoach();                     // before audio::shutdown(): the TTS worker joins
    void updateCoach(float dt);               // once per frame (inside simulate, after the animators)
    void updateCoachInput();                  // Playing: Esc menu, the HUD, Space, Backspace, input
    void updateCoachGameOver();               // GameOver: Space skips the appraisal, the HUD
    void coachMoveCompleted();                // completeMove(): after the move is recorded
    void coachGameOver();                     // endGame()
    void endLesson();                         // the rules lesson is over: its own ending
    bool coachEndCardReady() const;           // GameOver: the coach has said everything
    bool coachHandshakeWanted() const;        // GameOver: the closing words are said
    bool coachHoldsMove() const;              // updateAi: the coach's move waits (review, hands, draw offer)
    bool coachMayTouch() const;               // the player may touch a piece now
    void coachPlayerTouched();                // humanTouch(): the session hears of it
    void coachIllegalAttempt(chess::Square from, chess::Square to);
    bool coachBookMove(chess::Move& mv);      // updateAi: the teaching repertoire's move, if any
    void coachGazeTarget(m::vec3& target);    // updateGaze: where the coach looks while it talks
    // render(): the pieces the coach designates and its marks on the board, this frame.
    void coachMarks(std::vector<PieceHighlight>& highlights, std::vector<CoachMark>& marks);
    void drawCoachSubtitles();                // renderOverlay()
    void persistCoachResults();               // Settings [coach] from the session (once per game)
    bool coachCanTakeBack() const;            // the Esc menu's "Take back" (session and scoresheets)
    void coachOfferDraw();                    // the Esc menu: the coach answers after an analysis
    void coachEvaluateDraw();                 // that analysis, of the position on the board
    void coachPauseMenuFrame();               // the Esc menu of a coach game (paused_)
    void coachHudFrame();                     // the takeback card and the skip hint, their answers
    void runCoachTable(float dt);             // the coach's table jobs (demonstrations, takebacks...)
    void runStageTest(float dt);              // --coach-stage-test

    // ---- saved games (game_archive.h, game_saving.h) ----
    // Saves the game being played in the folder of saved games, once (archived_): at its end, when
    // the player leaves it, and as a safety net (the menu, the window closed). 'finished': it has
    // a result (a direct match: what its authority reports, see saving::directMatchRecord).
    void archiveGame(bool finished);

    // ---- replay of a saved game (GameMode::Replay) ----
    bool loadReplay(const std::string& path, int game);   // replayRecord_ from a file, false (logged) when it cannot be read
    void setupReplay();                       // part of setupNewGame(): the replay clock, the sheets' header
    Scorekeeper::Details replaySheetDetails() const;   // the record's Event and Round
    std::string replaySheetDate() const;      // its Date as a scoresheet writes dates
    void configureReplaySeats();              // the players of the record (names, Elo) in robot seats
    chess::TimeControl replayTimeControl() const;   // the record's: untimed without TimeControl or clocks
    void updateReplay(float dt);              // updatePlaying() while replaying: the replay clock drives the robots
    // The robot of 'seat' plays 'mv' on the board: touch, carry, capture, castling rook, promotion
    // swap, clock press (untimed: completed as its last piece is released). Stockfish's moves and
    // the replay's.
    void playRobotMove(int seat, const chess::Move& mv);
    void setReplayPosition(int ply);          // the board, the game and the sheets at 'ply', at once
    void completeReplayMove(int seat, const chess::Arbiter::Verdict& v);   // completeMove() of a replay
    void endReplay();                         // the record's last move is played: its result card
    ui::GameOverExtras replayGameOverExtras() const;   // the record's players and result, "Replay again"
    // A record that starts with Black to move (a FEN game, "40... Kd7"): 1, its moves go one ply
    // further on the sheets and in the move list, after White's cell left with "..."; else 0.
    int replaySheetOffset() const;
    void updateReplayInput();
    bool replayKey(const std::string& key);   // "K", "J", "L", "Shift+J", "Shift+L", "Home", "End"
    void drawReplayBar();                     // the replay's buttons, speed and move counter
    ClockDisplay replayClockDisplay() const;  // the record's clocks (replayClock_), dashes without them

    // ---- Analysis mode (GameMode::Analysis, game_scene_analysis.cpp) ----
    friend class AnalysisStage;
    AnalysisRuntime& analysisRuntime();       // created on first use
    // replayRecord_ from the Analysis page's choice (a file and its game, or the PGN text itself),
    // false (noticed) when it cannot be read.
    bool loadAnalysis(const ui::ReplaySetup& choice);
    // The game just played (its game over card's "Analyse the game"): declines the rematch, leaves
    // the game (an online one, the coach's), and fades over to its analysis, at its last position.
    void analyseGameJustPlayed();
    // The game being played as a record of the saved games: names, ratings, times, the ending
    // ('finished': it has a result; a direct match: its authority's moves and ending). False for a
    // direct match its authority aborted.
    bool playedRecord(chess::pgn::Record& out, bool& finished, archive::Mode& mode) const;
    void setupAnalysis();                     // part of setupNewGame(): the review, the cache, the voice offer
    void updateAnalysis(float dt);            // updatePlaying() while analysing: steps, review, comments
    void holdAnalysis();                      // in its place while the pause menu is open: the voice waits
    bool analysisLoaded() const;              // the analysed game is set (its clocks, its marks)
    void analysisOptionsChanged();            // the options' language and subtitles for the commentator
    void updateAnalysisInput();               // J K L, Home End, N M B (besides the viewer's keys)
    bool analysisKey(const std::string& key); // a key, or a --replay-keys entry ("Goto:12")
    void analysisGoTo(int position);          // the board to that position: animated when adjacent, else set at once
    // A step's move (forward) or takeback is over: the marks of the new position, its comment.
    void analysisStepDone(bool forward);
    void completeAnalysisMove(int seat, const chess::Arbiter::Verdict& v);   // completeMove() of an analysis
    void drawAnalysisOverlay();               // renderOverlay(): the bar, the move list, the subtitles
    // render(): the review's symbols, tint and better-move arrow on the board, and the
    // commentator's marks (in the coach's colours).
    void analysisMarks(std::vector<AnalysisMark>& marks, std::vector<PieceHighlight>& highlights,
                       std::vector<CoachMark>& coachMarks);
    void leaveAnalysis();                     // the menu, a new game, shutdown: the cache saved, silence
    ClockDisplay analysisClockDisplay() const;   // the record's clocks at the position (nothing runs)
    const std::vector<std::string>& analysedMoves() const;   // the whole game's SAN (the sheets)

    // ---- viewer mode ----
    bool observerView() const;                // the observer camera is the view
    void updateWatchInput();                  // once per frame: menu, viewpoints, overlay
    void updateObserver(float dt);            // the observer camera (inside simulate)
    CameraPose viewpoint(int n) const;        // presets 1..9 (0 = eyes of the player to move)
    CameraPose eyePose(int seat) const;
    void selectViewpoint(int n, bool jump);
    // A move completed while watching through the players' eyes: the view flies to the other
    // player's (the hot-seat handover; --handover-preview also freezes the clock until it lands).
    void followEyesAfterMove(int seat);
    int headNearCamera(m::vec3 p) const;      // seat whose head contains p (drawn headless), or -1
    // Analysis mode: the seat whose head stands between the camera at p and the board's centre (a
    // robot leaning in to play, seen from behind it: drawn headless, the board stays in view), or -1.
    int headBeforeBoard(m::vec3 p) const;
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
    double timeSum_ = 0.0;  // time_ summed in double: float += dt drifts within hours, then stops
    bool paused_ = false;
    bool startWatching_ = false;  // --viewer
    bool skipIntro_ = false;
    bool warpDone_ = false;
    float warpLeft_ = 0.0f;             // --warp time still to run after a --play-then menu choice
    chess::Color humanColor_ = chess::White;
    ui::NewGameSetup setup_;
    ui::WatchSetup watch_;
    bool debugCamera_ = false;    // --cam in a human game
    bool analysisMarksTest_ = false;   // --analysis-marks-test

    // Touch / move state
    int touchedId_ = -1;
    chess::Square touchedSq_ = chess::NoSquare;
    chess::Square placedTo_ = chess::NoSquare;
    bool pressQueued_ = false;
    bool drawOfferPending_ = false;           // offered while my move waits for the clock press: answered
                                              // once it is pressed (FIDE 9.1.2)
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
    float clockDt_ = 0.0f;              // what the clock counts in this simulate(): AppContext::clockDt, a warp's step
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
        // Standing (stance_control.h): 0 = the seated base look, 1 = the look at the board's
        // centre from the eyes; yaw and pitch are offsets from that base.
        float standBlend = 0.0f;
    };
    Look look_[2];
    // S: the player whose eyes are the view looks at their own scoresheet. One state for the
    // view, not per seat: in a hot-seat game a turn's look ends with the move (startHandover).
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

    // Standing up (stance_control.h)
    bool hotSeatHold_[2] = {false, false};   // hot-seat's own hold of each scoresheet (with the stance's)
    bool stoodThisGame_ = false;             // the first stand of a game has had its notice
    stance::NoticeLimiter sitNotice_{2.5f};   // "Sit back down to play"
    stance::NoticeLimiter busyNotice_{2.5f};  // "Finish your move first"
    anim::Stance stanceArg_ = anim::Stance::Seated;   // --stance
    bool stanceArgPending_ = false;           // --stance not applied yet (once per run)

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
    int drawOfferBy_ = -1;              // seat whose draw offer goes with its next move (completeMove)
    int drawCardFor_ = -1;              // seat asked to accept a draw (card)
    float captionAge_ = 1e9f;           // "Bob, your move" (since the handover began)
    float handoverArg_ = -1.0f;         // --handover <s> (this session only); -1 = Options > Gameplay
    float scriptWait_ = 0.0f;           // --play: a short pause before each scripted move
    int hsEloBefore_[2] = {0, 0}, hsEloAfter_[2] = {0, 0};
    // --play: moves made by hand by the human player(s), one per turn
    std::vector<std::string> script_;
    size_t scriptPos_ = 0;
    chess::PieceType scriptPromo_ = chess::NoPiece;
    // --play-then: what the player picks in the Esc menu once the moves of --play are made
    std::vector<std::string> scriptThen_;
    size_t scriptThenPos_ = 0;
    ui::MenuAction scriptMenu_ = ui::MenuAction::None;   // chosen in the Esc menu this frame

    // Saved games: the time each move took, by ply (the clock's own count: pauses and hot-seat
    // handovers excluded), and the mover's clock after it (-1 untimed, or a move set up by --moves)
    std::vector<int64_t> moveElapsedMs_, moveClockMs_;
    double plyElapsedMs_ = 0.0;         // the turn being played, so far
    std::time_t gameStartedAt_ = 0;     // the first turn began (Date and Time tags)
    bool archived_ = false;             // archiveGame() ran for this game
    bool directMatch_ = false;          // the online game is a direct match (link_ goes before saving)
    ui::LibrarySetup library_;          // the title page's "Saved games" entry

    // Replay (GameMode::Replay)
    chess::pgn::Record replayRecord_;   // the game replayed
    replay::ReplayClock replayClock_;   // when its moves are played, what its clocks show
    replay::Speed replaySpeedArg_ = replay::Speed::X1;   // --replay-speed
    bool replayPausedArg_ = false;      // --replay-paused
    float replayMoveAt_ = 0.0f;         // time_ when the robot began the move being played (log)
    std::vector<std::string> replayKeys_;   // --replay-keys
    size_t replayKeysPos_ = 0;
    float replayKeyWait_ = 0.0f;        // seconds before the next --replay-keys key

    // UI
    bool showMoveList_ = false;
    bool gameOverShown_ = false;
    bool endHandshakeDone_ = false;
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
    ClockDisplay leaveClock_;           // the clock as the game was left (the fade to the menu)
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
    live::StanceTracker stanceTracker_; // the opponent's stance (OpponentStance events)
    anim::Stance remoteShown_ = anim::Stance::Seated;   // the stance their robot is given (updateStances)
    int mockStance_ = -1;               // --mock-stance / F11: the fake opponent's held stance (-1: its own)
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
    bool reportQueued_ = false;         // filled in during the game: sent once it has ended
    bool reportSending_ = false;        // sent, its ReportResult awaited
    int reportCategory_ = 0;
    std::string reportComment_;
    std::string startOnline_;           // --start-online category
    std::string startTouch_;            // --start-online with --touch <square>: touched once idle
    float fadeDip_ = 0.0f;              // short darkening while the board is rebuilt

    // Coach mode
    CoachArgs coachArgs_;               // --start --coach and its options
    ui::CoachSetup coachSetup_;         // the coach page's choice
    int coachLevel_ = 1;                // the coach game being played: 0 = the rules lesson, 1..6
    float coachFaceLift_ = 0.0f;        // 0..1: the view rises gently to the coach's face as it talks to the player
    float coachFade_ = 0.0f;            // a lesson position being set up behind a fade
    bool coachVoiceFiles_ = false;      // tts::modelFilesPresent() at start-up
    std::unique_ptr<CoachRuntime> coach_;

    // Analysis mode
    struct AnalysisRuntimeDelete {
        void operator()(AnalysisRuntime* r) const;   // game_scene_analysis.cpp (a type complete there only)
    };
    std::unique_ptr<AnalysisRuntime, AnalysisRuntimeDelete> analysis_;
    bool analysisAtEnd_ = false;        // the analysis opens at the last position (a game just played)
    int analysisAtArg_ = -1;            // --analysis-at N: it opens at position N
    bool analysisWhiteBottom_ = true;   // the bar and the camera from White's side (false: the
                                        // player had Black in the game just played)
};

}  // namespace game
