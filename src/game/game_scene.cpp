#include "game_scene.h"
#include "../audio/audio.h"
#include "../character/skeleton.h"
#include "../core/log.h"
#include "../platform/platform.h"
#include "../render/post/postfx.h"
#include "layout.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace m;
using namespace chess;

namespace game {

namespace {

constexpr float kFadeOut = 0.8f;          // menu -> black
constexpr float kFadeIn = 1.6f;           // black -> seated at the table
constexpr float kBaseGazePitch = -0.62f;  // looking down at the board from the chair
constexpr float kFov = 52.0f * DEG;
constexpr float kHeadYawLimit = 70.0f * DEG;
constexpr float kHeadPitchDown = -45.0f * DEG, kHeadPitchUp = 30.0f * DEG;
constexpr float kEyeLimit = 18.0f * DEG;

anim::Task task(anim::TaskType t, int pieceId = -1, vec3 pos = vec3(0), float height = 0.0f) {
    anim::Task k;
    k.type = t;
    k.pieceId = pieceId;
    k.position = pos;
    k.height = height;
    return k;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

float wrapAngle(float a) {
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

float yawOf(const mat4& t) {
    vec3 z = t.c[2].xyz();
    return std::atan2(z.x, z.z);
}

const char* pieceName(PieceType t) {
    switch (t) {
    case Pawn: return "pawn";
    case Knight: return "knight";
    case Bishop: return "bishop";
    case Rook: return "rook";
    case Queen: return "queen";
    case King: return "king";
    default: return "piece";
    }
}

float distPointSegment2D(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 1e-9f ? clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

bool GameScene::init(AppContext& ctx) {
    ctx_ = &ctx;
    rng_.seedWith(ctx.screenshotMode ? 20260927u : plat::randomSeed());
    demo_ = ctx.hasArg("--demo");
    skipIntro_ = ctx.hasArg("--no-intro");
    viewOverride_ = ctx.argValue("--view");

    Settings& s = settings();
    setup_.difficulty = s.difficultyPreset;
    setup_.timeControl = s.timeControlPreset;
    setup_.customBaseSeconds = s.customBaseSeconds;
    setup_.customIncrementSeconds = s.customIncrementSeconds;
    setup_.customDelaySeconds = s.customDelaySeconds;
    setup_.skillLevel = s.customSkillLevel;
    setup_.limitElo = s.customLimitElo;
    setup_.elo = s.customElo;
    setup_.depth = s.customDepth;
    setup_.moveTimeMs = s.customMoveTimeMs;
    setup_.nodes = s.customNodes;

    if (!audio::init()) LOGW("audio unavailable, continuing silently");
    if (!ui::init()) {
        LOGE("UI initialisation failed");
        return false;
    }
    std::vector<ui::DifficultyInfo> diff;
    for (const ai::Preset& p : ai::presets()) diff.push_back({p.name, p.description, p.approxElo});
    ui::setDifficultyList(diff);
    std::vector<std::string> tcs;
    for (const TimeControl& tc : timeControlPresets()) tcs.push_back(tc.label());
    ui::setTimeControlList(tcs);
    applySettings(false);

    state_ = State::Loading;
    fade_ = 1.0f;
    if (ctx.screenshotMode) {
        // Deterministic runs load everything up front.
        while (!world_.loadStep()) {}
        finishLoading();
    }
    return true;
}

void GameScene::finishLoading() {
    world_.setupRenderer(*ctx_->renderer);
    engineOk_ = engine_.start();
    if (!engineOk_) LOGW("Stockfish is unavailable: the opponent will play random legal moves");
    initAnimators();
    board_.reset(true);
    world_.setClockSide(true);
    clock_.setup(chosenTimeControl());
    if (ctx_->hasArg("--start") || demo_) {
        setupNewGame();
        if (skipIntro_) {
            fade_ = 0.0f;
            startPlaying();
        } else {
            state_ = State::Intro;
            stateTime_ = 0.0f;
        }
        std::string touch = ctx_->argValue("--touch");
        if (!touch.empty() && state_ == State::Playing && turn_ == Turn::HumanIdle) {
            int id = board_.idAt(parseSquare(touch));
            if (id >= 0) humanTouch(id);
        }
    } else {
        enterMenu();
    }
}

void GameScene::initAnimators() {
    const character::Skeleton& sk = character::robotSkeleton();
    for (int seat = 0; seat < 2; ++seat) {
        float zs = seat == 0 ? 1.0f : -1.0f;  // White at +Z faces -Z
        anim_[seat] = anim::Animator();
        anim_[seat].init(sk, vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs);
        // Right hand rests on the table beside the board (White's right is +X, Black's is -X).
        anim_[seat].setRestHand(vec3(zs * 0.265f, layout::TABLE_TOP_Y, zs * 0.305f));
        anim_[seat].pieceTransform = [this](int id) {
            const PieceObject* p = board_.byId(id);
            return p ? p->transform : mat4();
        };
        anim_[seat].pieceGripInfo = [this](int id) {
            const PieceObject* p = board_.byId(id);
            int t = p ? int(p->type) : int(Pawn);
            // (height, grip height above the base, grip radius), metres
            return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_HEIGHT[t] * layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
        };
        hasPrevGlobals_[seat] = false;
    }
}

void GameScene::enterMenu() {
    state_ = State::Menu;
    stateTime_ = 0.0f;
    turn_ = Turn::None;
    paused_ = false;
    game_.reset();
    arbiter_.reset(game_);
    board_.reset(true);
    world_.setClockSide(true);
    clock_.setup(chosenTimeControl());
    leverSide_ = leverTarget_ = -1.0f;
    dest_.clear();
    initAnimators();
    for (auto& a : anim_) a.setHeadOverride(false);
    menuAngle_ = 0.9f;
    plat::setMouseCaptured(false);
    dragging_ = false;
}

TimeControl GameScene::chosenTimeControl() const {
    const auto& presets = timeControlPresets();
    if (setup_.timeControl >= 0 && setup_.timeControl < int(presets.size())) return presets[size_t(setup_.timeControl)];
    TimeControl tc;
    tc.unlimited = false;
    tc.baseMs = int64_t(std::max(10, setup_.customBaseSeconds)) * 1000;
    tc.incrementMs = int64_t(std::max(0, setup_.customIncrementSeconds)) * 1000;
    tc.delayMs = int64_t(std::max(0, setup_.customDelaySeconds)) * 1000;
    return tc;
}

ai::EngineSettings GameScene::chosenEngineSettings() const {
    const auto& presets = ai::presets();
    int idx = std::clamp(setup_.difficulty, 0, int(presets.size()) - 1);
    ai::EngineSettings es = presets[size_t(idx)].settings;
    if (idx == int(presets.size()) - 1) {  // "Custom"
        es.skillLevel = setup_.skillLevel;
        es.limitStrength = setup_.limitElo;
        es.elo = setup_.elo;
        es.depth = setup_.depth;
        es.moveTimeMs = setup_.moveTimeMs;
        es.nodes = setup_.nodes;
    }
    es.threads = std::max(1, settings().engineThreads);
    es.hashMB = std::max(16, settings().engineHashMB);
    es.humanize = settings().humanizeThinking && !ctx_->screenshotMode;
    return es;
}

void GameScene::setupNewGame() {
    Settings& s = settings();
    humanColor_ = s.nextColor < 0 ? (rng_.uniform() < 0.5f ? White : Black) : Color(s.nextColor & 1);
    std::string forced = ctx_->argValue("--human");
    if (forced == "white") humanColor_ = White;
    if (forced == "black") humanColor_ = Black;
    LOGI("New game: human plays %s, %s, difficulty %d", humanColor_ == White ? "White" : "Black",
         chosenTimeControl().label().c_str(), setup_.difficulty);

    game_.reset();
    arbiter_.reset(game_);
    bool clockPosX = humanColor_ == White;  // at the human player's right hand
    world_.setClockSide(clockPosX);
    board_.reset(clockPosX);
    clock_.setup(chosenTimeControl());
    clockAccumMs_ = 0.0;
    // White's clock runs first, as if Black had pressed: Black's half of the lever is down.
    leverSide_ = leverTarget_ = world_.clockHalfForSeat(-1.0f) == 1 ? 1.0f : -1.0f;

    dest_.clear();
    turn_ = Turn::None;
    paused_ = false;
    touchedId_ = -1;
    touchedSq_ = placedTo_ = NoSquare;
    pressQueued_ = false;
    drawOfferPly_ = -1;
    lastAiEval_ = 0;
    gameOverShown_ = false;
    showMoveList_ = false;

    if (engineOk_) {
        engine_.newGame();
        engine_.configure(chosenEngineSettings());
    }
    initAnimators();
    anim_[humanSeat()].setHeadOverride(true, 0.0f, kBaseGazePitch);
    anim_[aiSeat()].setHeadOverride(false);
    lookYaw_ = lookPitch_ = 0.0f;
    gazeYaw_ = 0.0f;
    gazePitch_ = kBaseGazePitch;
    lean_ = leanSmooth_ = 0.0f;

    std::string moves = ctx_->argValue("--moves");
    if (!moves.empty()) applyMovesInstantly(split(moves, ','));
}

void GameScene::applyMovesInstantly(const std::vector<std::string>& uci) {
    for (const std::string& u : uci) {
        Move mv = game_.position().parseUCI(u);
        if (!mv.valid()) {
            LOGW("--moves: '%s' is not legal here", u.c_str());
            break;
        }
        game_.play(mv);
    }
    board_.syncTo(game_.position());
    arbiter_.reset(game_);
}

void GameScene::startPlaying() {
    state_ = State::Playing;
    stateTime_ = 0.0f;
    // Colours alternate from one game to the next.
    settings().nextColor = int(opposite(humanColor_));
    settings().save();
    if (game_.status() != GameStatus::Ongoing) {
        endGame();
        return;
    }
    clock_.start(game_.position().sideToMove());
    audio::playUI(audio::Sfx::GameStart, 0.6f);
    beginTurn();
}

void GameScene::beginTurn() {
    Color stm = game_.position().sideToMove();
    touchedId_ = -1;
    touchedSq_ = placedTo_ = NoSquare;
    pressQueued_ = false;
    hoverId_ = -1;
    if (!demo_ && stm == humanColor_) {
        turn_ = Turn::HumanIdle;
    } else {
        turn_ = Turn::AiThinking;
        aiRequested_ = false;
    }
}

void GameScene::endGame() {
    // A piece still gripped on its square is let go.
    if (turn_ == Turn::HumanTouched && touchedId_ >= 0) {
        PieceObject* p = board_.byId(touchedId_);
        if (p) {
            dest_[p->id].push_back({touchedSq_, p->basePos, false});
            anim_[humanSeat()].enqueue({task(anim::TaskType::Place, p->id, p->basePos), task(anim::TaskType::Retract)});
        }
    }
    clock_.stop();
    turn_ = Turn::None;
    state_ = State::GameOver;
    stateTime_ = 0.0f;
    gameOverShown_ = false;
    endHandshakeDone_ = false;
    paused_ = false;
    for (auto& a : anim_) a.setThinking(false);
    GameStatus st = game_.status();
    resultText_ = st == GameStatus::WhiteWins ? "1-0" : st == GameStatus::BlackWins ? "0-1" : "\xC2\xBD-\xC2\xBD";
    reasonText_ = endReasonText(game_.endReason());
    isDraw_ = st == GameStatus::Draw;
    playerWon_ = (st == GameStatus::WhiteWins && humanColor_ == White) || (st == GameStatus::BlackWins && humanColor_ == Black);
    LOGI("Game over: %s (%s)\n%s", resultText_.c_str(), reasonText_.c_str(),
         game_.pgn(humanColor_ == White ? "Player" : "Scacelith", humanColor_ == White ? "Scacelith" : "Player").c_str());
    audio::playUI(audio::Sfx::GameEnd, 0.7f);
}

void GameScene::applySettings(bool displayToo) {
    Settings& s = settings();
    if (ctx_ && ctx_->renderer) {
        ctx_->renderer->setSettings(s.renderSettings());
        ctx_->renderer->post().settings.exposureCompensation = s.brightness;
    }
    audio::setMasterVolume(s.masterVolume);
    audio::setEffectsVolume(s.effectsVolume);
    audio::setAmbienceVolume(s.ambienceVolume);
    audio::setAmbienceEnabled(s.ambience);
    if (displayToo) {
        plat::setDisplayMode(s.fullscreen ? plat::DisplayMode::Borderless : plat::DisplayMode::Windowed, s.displayWidth,
                             s.displayHeight);
        plat::setVsync(s.vsync);
        s.save();
    }
}

void GameScene::shutdown(AppContext&) {
    engine_.shutdown();
    ui::shutdown();
    audio::shutdown();
}

// =============================================================================================
// Frame update
// =============================================================================================

bool GameScene::update(AppContext& ctx, float dt) {
    if (!warpDone_) {
        warpDone_ = true;
        float warp = float(std::atof(ctx.argValue("--warp", "0").c_str()));
        if (warp > 0.0f && state_ != State::Loading) runWarp(warp);
    }
    const plat::Input& in = plat::input();
    ui::beginFrame(plat::width(), plat::height(), dt);
    bool keepRunning = true;

    switch (state_) {
    case State::Loading:
        ui::loadingScreen(world_.loadProgress(), world_.loadLabel());
        if (world_.loadStep()) finishLoading();
        break;
    case State::Menu: {
        fade_ = std::max(0.0f, fade_ - dt / kFadeIn);
        ui::MenuAction a = ui::mainMenu(setup_);
        if (a == ui::MenuAction::StartGame) {
            Settings& s = settings();
            s.difficultyPreset = setup_.difficulty;
            s.timeControlPreset = setup_.timeControl;
            s.customBaseSeconds = setup_.customBaseSeconds;
            s.customIncrementSeconds = setup_.customIncrementSeconds;
            s.customDelaySeconds = setup_.customDelaySeconds;
            s.customSkillLevel = setup_.skillLevel;
            s.customLimitElo = setup_.limitElo;
            s.customElo = setup_.elo;
            s.customDepth = setup_.depth;
            s.customMoveTimeMs = setup_.moveTimeMs;
            s.customNodes = setup_.nodes;
            s.save();
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        } else if (a == ui::MenuAction::Quit) {
            keepRunning = false;
        } else if (a == ui::MenuAction::OptionsChanged) {
            applySettings(true);
        }
        break;
    }
    case State::Playing: {
        bool optionsOpen = ui::optionsOpen();
        if (in.keyPressed[plat::KEY_ESCAPE] && !optionsOpen) {
            paused_ = !paused_;
            if (paused_ && dragging_) {
                dragging_ = false;
                plat::setMouseCaptured(false);
            }
        }
        if (paused_) {
            bool canClaim = game_.canClaimThreefold() || game_.canClaimFiftyMove();
            switch (ui::pauseMenu(canClaim)) {
            case ui::MenuAction::Resume: paused_ = false; break;
            case ui::MenuAction::Resign:
                paused_ = false;
                game_.resign(humanColor_);
                endGame();
                break;
            case ui::MenuAction::OfferDraw:
                paused_ = false;
                offerDraw();
                break;
            case ui::MenuAction::ClaimDraw:
                paused_ = false;
                game_.claimDraw();
                if (game_.status() != GameStatus::Ongoing) endGame();
                else ui::notify("No draw can be claimed in this position.");
                break;
            case ui::MenuAction::BackToMainMenu:
                paused_ = false;
                if (game_.status() == GameStatus::Ongoing) game_.resign(humanColor_);
                clock_.stop();
                state_ = State::FadeToMenu;
                stateTime_ = 0.0f;
                break;
            case ui::MenuAction::OptionsChanged: applySettings(true); break;
            default: break;
            }
        } else {
            if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
            if (!demo_ && isHumanTurn()) updateHumanInput();
        }
        break;
    }
    case State::GameOver:
        if (stateTime_ > 1.2f && (endHandshakeDone_ || stateTime_ > 5.0f)) {
            gameOverShown_ = true;
            ui::MenuAction a = ui::gameOver(resultText_, reasonText_, playerWon_, isDraw_);
            if (a == ui::MenuAction::Rematch) {
                state_ = State::FadeToGame;
                stateTime_ = 0.0f;
            } else if (a == ui::MenuAction::BackToMainMenu) {
                state_ = State::FadeToMenu;
                stateTime_ = 0.0f;
            }
        }
        if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
        break;
    default: break;
    }

    simulate(dt);
    return keepRunning;
}

void GameScene::runWarp(float seconds) {
    const float step = 1.0f / 60.0f;
    LOGI("warping %.1f s of game time", seconds);
    for (float t = 0.0f; t < seconds; t += step) {
        // The engine searches in real time: wait for it so the warp stays deterministic.
        if (state_ == State::Playing && turn_ == Turn::AiThinking && aiRequested_ && engineOk_) {
            for (int i = 0; i < 6000 && !engine_.moveReady(); ++i) plat::sleepMs(5);
        }
        if (state_ == State::GameOver && stateTime_ > 1.0f) break;
        simulate(step);
    }
}

void GameScene::simulate(float dt) {
    time_ += dt;
    stateTime_ += dt;
    board_.beginFrame();

    switch (state_) {
    case State::FadeToGame:
        fade_ = std::min(1.0f, fade_ + dt / kFadeOut);
        if (fade_ >= 1.0f && stateTime_ > kFadeOut + 0.25f) {
            setupNewGame();
            state_ = skipIntro_ ? State::Handshake : State::Intro;
            stateTime_ = 0.0f;
            if (skipIntro_) fade_ = 0.0f;
        }
        break;
    case State::Intro:
        fade_ = std::max(0.0f, 1.0f - stateTime_ / kFadeIn);
        if (stateTime_ >= kFadeIn * 0.75f) {
            // Handshake across the board before the first move.
            anim::Task h0 = task(anim::TaskType::Handshake), h1 = h0;
            h0.partner = &anim_[1];
            h1.partner = &anim_[0];
            anim_[0].enqueue(h0);
            anim_[1].enqueue(h1);
            state_ = State::Handshake;
            stateTime_ = 0.0f;
        }
        break;
    case State::Handshake:
        fade_ = std::max(0.0f, fade_ - dt / kFadeIn);
        if (stateTime_ > 0.3f && !anim_[0].busy() && !anim_[1].busy()) startPlaying();
        break;
    case State::Playing:
        if (!paused_) updatePlaying(dt);
        break;
    case State::GameOver:
        if (!endHandshakeDone_ && stateTime_ > 0.8f && !anim_[0].busy() && !anim_[1].busy()) {
            anim::Task h0 = task(anim::TaskType::Handshake), h1 = h0;
            h0.partner = &anim_[1];
            h1.partner = &anim_[0];
            anim_[0].enqueue(h0);
            anim_[1].enqueue(h1);
            endHandshakeDone_ = true;
        }
        break;
    case State::FadeToMenu:
        fade_ = std::min(1.0f, fade_ + dt / kFadeOut);
        if (fade_ >= 1.0f && stateTime_ > kFadeOut + 0.25f) enterMenu();
        break;
    default: break;
    }

    // Clock lever swings quickly to the pressed side.
    float leverSpeed = 1.0f / 0.07f;
    leverSide_ += clamp(leverTarget_ - leverSide_, -leverSpeed * dt, leverSpeed * dt);

    // Characters (frozen while the game is paused).
    bool frozen = paused_ && state_ == State::Playing;
    bool firstPerson = state_ != State::Menu && state_ != State::Loading && state_ != State::FadeToGame;
    if (state_ == State::FadeToGame) firstPerson = false;
    updateCamera(dt, firstPerson);  // sets the player's head override before the animation update
    updateGaze(dt);
    if (!frozen && state_ != State::Loading) {
        for (int seat = 0; seat < 2; ++seat) {
            events_.clear();
            anim_[seat].update(dt, events_);
            handleEvents(seat, events_);
        }
    }
    // Pieces in a hand follow it; the others rest where they were put.
    for (PieceObject& p : board_.pieces()) {
        if (!p.held) continue;
        mat4 t;
        if (anim_[0].heldPieceTransform(p.id, t) || anim_[1].heldPieceTransform(p.id, t)) p.transform = t;
    }
    board_.updateRestingTransforms();
}

bool GameScene::isHumanTurn() const {
    return turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing ||
           turn_ == Turn::HumanPromotion || turn_ == Turn::HumanPlaced || turn_ == Turn::HumanPressing;
}

void GameScene::updatePlaying(float dt) {
    // Clock
    if (clock_.isRunning()) {
        clockAccumMs_ += double(dt) * 1000.0;
        int64_t ms = int64_t(clockAccumMs_);
        clockAccumMs_ -= double(ms);
        clock_.update(ms);
        Color r = clock_.running();
        if (!clock_.timeControl().unlimited && clock_.flagged(r)) {
            game_.flagFall(r);
            ui::notify(r == humanColor_ ? "Your flag has fallen." : "Your opponent's flag has fallen.", 4.0f);
            endGame();
            return;
        }
    }
    switch (turn_) {
    case Turn::HumanPlacing:
        if (dest_.empty() && !anim_[humanSeat()].busy()) {
            if (arbiter_.pendingNeedsPromotion(game_)) {
                turn_ = Turn::HumanPromotion;
            } else {
                turn_ = Turn::HumanPlaced;
                if (pressQueued_) humanPressClock();
            }
        }
        break;
    case Turn::HumanPromotion: {
        int choice = ui::promotionPicker(humanColor_ == White);
        if (choice >= Knight && choice <= Queen) {
            PieceType t = PieceType(choice);
            arbiter_.choosePromotion(game_, t);
            std::vector<anim::Task> tasks;
            planPromotionSwap(tasks, board_.idAt(placedTo_), placedTo_, t);
            anim_[humanSeat()].enqueue(tasks);
            turn_ = Turn::HumanPlacing;
        }
        break;
    }
    case Turn::AiThinking: updateAi(dt); break;
    default: break;
    }
}

// =============================================================================================
// Human player
// =============================================================================================

void GameScene::updateHumanInput() {
    const plat::Input& in = plat::input();
    Ray ray = mouseRay();
    float tPiece = 1e30f;
    int pid = pickPiece(ray, &tPiece);
    const PieceObject* hovered = pid >= 0 ? board_.byId(pid) : nullptr;
    hoverId_ = (hovered && hovered->color == humanColor_ && (turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched)) ? pid : -1;

    if (in.keyPressed[plat::KEY_SPACE] && !ui::wantsKeyboard()) {
        humanPressClock();
        return;
    }
    if (!in.mousePressed[plat::MOUSE_LEFT] || ui::wantsMouse() || dragging_) return;

    float tClock = 1e30f;
    if (world_.rayHitsClock(ray, &tClock) && tClock < tPiece) {
        humanPressClock();
        return;
    }
    PieceObject* p = pid >= 0 ? board_.byId(pid) : nullptr;
    Square sq = p ? p->square : pickSquare(ray);

    switch (turn_) {
    case Turn::HumanIdle:
        if (p && p->color == humanColor_) humanTouch(p->id);
        break;
    case Turn::HumanTouched: {
        PieceObject* touched = board_.byId(touchedId_);
        bool sameSquare = (p && p->id == touchedId_) || (!p && sq == touchedSq_);
        if (sameSquare) {
            if (!arbiter_.touchedHasLegalMove(game_)) {
                humanRelease();
            } else {
                ui::notify(std::string("Touch-move: the ") + pieceName(touched->type) + " you touched must be moved.", 3.0f);
            }
        } else if (p && p->color == humanColor_) {
            // Castling by pointing at the rook once the king is in hand.
            if (touched && touched->type == King && p->type == Rook && rankOf(p->square) == rankOf(touchedSq_)) {
                int file = fileOf(p->square) > fileOf(touchedSq_) ? 6 : 2;
                Square to = makeSquare(file, rankOf(touchedSq_));
                if (game_.position().findLegal(touchedSq_, to).valid()) {
                    humanPlace(to);
                    break;
                }
            }
            if (!arbiter_.touchedHasLegalMove(game_)) {
                humanRelease();
                humanTouch(p->id);
            } else {
                ui::notify(std::string("Touch-move: the ") + pieceName(touched->type) + " you touched must be moved.", 3.0f);
            }
        } else if (sq != NoSquare) {
            humanPlace(sq);
        }
        break;
    }
    case Turn::HumanPlaced: ui::notify("Press the clock to complete your move (Space).", 2.5f); break;
    default: break;
    }
}

void GameScene::humanTouch(int pieceId) {
    PieceObject* p = board_.byId(pieceId);
    if (!p || p->square == NoSquare) return;
    if (!arbiter_.touch(game_, p->square)) {
        Square committed = arbiter_.touchedSquare();
        ui::notify("Touch-move: you must move the piece on " + squareName(committed) + ".", 3.0f);
        return;
    }
    anim_[humanSeat()].enqueue(task(anim::TaskType::Reach, pieceId));
    touchedId_ = pieceId;
    touchedSq_ = p->square;
    turn_ = Turn::HumanTouched;
}

void GameScene::humanRelease() {
    PieceObject* p = board_.byId(touchedId_);
    if (!p) return;
    vec3 pos = board_.squareBase(touchedSq_);
    dest_[p->id].push_back({touchedSq_, pos, false});
    anim_[humanSeat()].enqueue({task(anim::TaskType::Place, p->id, pos), task(anim::TaskType::Retract)});
    arbiter_.cancelTouch();
    touchedId_ = -1;
    touchedSq_ = NoSquare;
    turn_ = Turn::HumanIdle;
}

void GameScene::humanPlace(Square to) {
    const Position& pos = game_.position();
    PieceObject* mover = board_.byId(touchedId_);
    if (!mover) return;
    PieceObject* occupant = board_.at(to);
    if (occupant && occupant->color == humanColor_) return;
    bool promo = mover->type == Pawn && (rankOf(to) == 7 || rankOf(to) == 0);
    Move mv = pos.findLegal(touchedSq_, to, promo ? Queen : NoPiece);
    if (!mv.valid() && settings().showLegalMoves) {
        ui::notify("Illegal move.", 2.0f);
        return;
    }
    if (!arbiter_.place(game_, to, NoPiece)) {
        ui::notify("That move cannot be made.", 2.0f);
        return;
    }
    int victimId = occupant ? occupant->id : -1;
    Square rookFrom = NoSquare, rookTo = NoSquare;
    if (mv.valid()) {
        if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(to + (humanColor_ == White ? -8 : 8)));
        if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
            int rank = rankOf(touchedSq_);
            bool king = (mv.flags & MoveCastleKing) != 0;
            rookFrom = makeSquare(king ? 7 : 0, rank);
            rookTo = makeSquare(king ? 5 : 3, rank);
        }
    }
    std::vector<anim::Task> tasks;
    planPlacement(tasks, mover->id, to, victimId, rookFrom, rookTo);
    anim_[humanSeat()].enqueue(tasks);
    placedTo_ = to;
    turn_ = Turn::HumanPlacing;
}

void GameScene::humanPressClock() {
    if (turn_ == Turn::HumanPlacing) {
        pressQueued_ = true;  // pressed as soon as the piece is down (not before a promotion)
        return;
    }
    if (turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched) {
        ui::notify("Make your move before pressing the clock.", 2.0f);
        return;
    }
    if (turn_ != Turn::HumanPlaced) return;
    int half = world_.clockHalfForSeat(humanSeat() == 0 ? 1.0f : -1.0f);
    anim_[humanSeat()].enqueue({task(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)), task(anim::TaskType::Retract)});
    pressQueued_ = false;
    turn_ = Turn::HumanPressing;
}

void GameScene::offerDraw() {
    int ply = int(game_.moves().size());
    if (drawOfferPly_ == ply) {
        ui::notify("You have already offered a draw this move.", 2.5f);
        return;
    }
    drawOfferPly_ = ply;
    bool accept = engineOk_ ? engine_.acceptsDraw(lastAiEval_, ply) : false;
    if (accept) {
        ui::notify("Your opponent accepts the draw.", 3.0f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify("Your opponent declines the draw offer.", 3.0f);
    }
}

// =============================================================================================
// Opponent (Stockfish)
// =============================================================================================

ai::ClockInfo GameScene::clockInfo() const {
    ai::ClockInfo ci;
    const TimeControl& tc = clock_.timeControl();
    ci.timed = !tc.unlimited;
    ci.whiteMs = clock_.remainingMs(White);
    ci.blackMs = clock_.remainingMs(Black);
    ci.whiteIncMs = ci.blackIncMs = tc.incrementMs;
    return ci;
}

void GameScene::updateAi(float dt) {
    Color side = game_.position().sideToMove();
    int seat = seatOf(side);
    if (!aiRequested_) {
        const Position& pos = game_.position();
        int legal = int(pos.legalMoves().size());
        if (engineOk_) {
            engine_.requestMove(game_.uciMoves(), clockInfo());
            aiThinkMs_ = engine_.thinkTimeMs(clockInfo(), int(game_.moves().size()), legal, pos.inCheck());
        } else {
            aiThinkMs_ = 900;
        }
        if (ctx_->screenshotMode) aiThinkMs_ = std::min(aiThinkMs_, 400);
        aiElapsed_ = 0.0f;
        aiRequested_ = true;
        anim_[seat].setThinking(true);
        return;
    }
    aiElapsed_ += dt;
    if (engineOk_ && !engine_.moveReady()) return;
    if (aiElapsed_ * 1000.0f < float(aiThinkMs_)) return;

    int evalCp = 0;
    std::string uci = engineOk_ ? engine_.takeMove(&evalCp) : std::string();
    lastAiEval_ = evalCp;
    Move mv = game_.position().parseUCI(uci);
    if (!mv.valid()) {
        std::vector<Move> legal = game_.position().legalMoves();
        if (legal.empty()) return;  // game end is detected when the previous move was played
        if (engineOk_) LOGW("engine returned '%s', playing a random move", uci.c_str());
        mv = legal[size_t(rng_.rangeInt(0, int(legal.size()) - 1))];
    }
    // The opponent claims a draw by repetition / fifty moves when it is not better.
    if ((game_.canClaimThreefold() || game_.canClaimFiftyMove()) && engineOk_ &&
        engine_.acceptsDraw(evalCp, int(game_.moves().size()))) {
        game_.claimDraw();
        if (game_.status() != GameStatus::Ongoing) {
            ui::notify("Your opponent claims a draw.", 3.0f);
            endGame();
            return;
        }
    }

    arbiter_.touch(game_, mv.from);
    arbiter_.place(game_, mv.to, mv.promotion);
    int moverId = board_.idAt(mv.from);
    int victimId = board_.idAt(mv.to);
    if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(mv.to + (side == White ? -8 : 8)));
    Square rookFrom = NoSquare, rookTo = NoSquare;
    if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
        int rank = rankOf(mv.from);
        bool king = (mv.flags & MoveCastleKing) != 0;
        rookFrom = makeSquare(king ? 7 : 0, rank);
        rookTo = makeSquare(king ? 5 : 3, rank);
    }
    std::vector<anim::Task> tasks;
    tasks.push_back(task(anim::TaskType::Reach, moverId));
    planPlacement(tasks, moverId, mv.to, victimId, rookFrom, rookTo);
    if (mv.promotion != NoPiece) planPromotionSwap(tasks, moverId, mv.to, mv.promotion);
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    tasks.push_back(task(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)));
    tasks.push_back(task(anim::TaskType::Retract));
    anim_[seat].setThinking(false);
    anim_[seat].enqueue(tasks);
    aiMoveTo_ = mv.to;
    turn_ = Turn::AiMoving;
}

// =============================================================================================
// Physical move planning
// =============================================================================================

vec3 GameScene::jitteredSquare(Square sq) {
    // Players never centre pieces perfectly.
    return board_.squareBase(sq) + vec3(rng_.range(-0.0016f, 0.0016f), 0.0f, rng_.range(-0.0016f, 0.0016f));
}

float GameScene::carryHeight(vec3 from, vec3 to, int ignoreA, int ignoreB) const {
    // Lift just enough to clear the pieces standing along the way.
    vec2 a(from.x, from.z), b(to.x, to.z);
    float top = 0.0f;
    for (const PieceObject& p : board_.pieces()) {
        if (p.id == ignoreA || p.id == ignoreB || p.held || p.square == NoSquare) continue;
        float d = distPointSegment2D(vec2(p.basePos.x, p.basePos.z), a, b);
        if (d < layout::PIECE_BASE_RADIUS[p.type] + 0.024f) top = std::max(top, layout::PIECE_HEIGHT[p.type]);
    }
    return std::max(0.022f, top + 0.014f);
}

void GameScene::planPlacement(std::vector<anim::Task>& tasks, int moverId, Square to, int victimId, Square rookFrom,
                              Square rookTo) {
    PieceObject* mover = board_.byId(moverId);
    if (!mover) return;
    vec3 toPos = jitteredSquare(to);
    tasks.push_back(task(anim::TaskType::Lift, moverId, vec3(0), carryHeight(mover->basePos, toPos, moverId, victimId)));
    tasks.push_back(task(anim::TaskType::Carry, moverId, toPos));
    PieceObject* victim = board_.byId(victimId);
    if (victim) tasks.push_back(task(anim::TaskType::TakeCaptured, victimId));
    tasks.push_back(task(anim::TaskType::Place, moverId, toPos));
    dest_[moverId].push_back({to, toPos, false});
    if (victim) {
        vec3 slot = board_.nextCaptureSlot(victim->color);
        tasks.push_back(task(anim::TaskType::Discard, victimId, slot));
        dest_[victimId].push_back({NoSquare, slot, true});
    }
    if (rookFrom != NoSquare) {
        int rookId = board_.idAt(rookFrom);
        PieceObject* rook = board_.byId(rookId);
        if (rook) {
            vec3 rp = jitteredSquare(rookTo);
            tasks.push_back(task(anim::TaskType::Reach, rookId));
            tasks.push_back(task(anim::TaskType::Lift, rookId, vec3(0), carryHeight(rook->basePos, rp, rookId, moverId)));
            tasks.push_back(task(anim::TaskType::Carry, rookId, rp));
            tasks.push_back(task(anim::TaskType::Place, rookId, rp));
            dest_[rookId].push_back({rookTo, rp, false});
        }
    }
}

void GameScene::planPromotionSwap(std::vector<anim::Task>& tasks, int pawnId, Square sq, PieceType newType) {
    PieceObject* pawn = board_.byId(pawnId);
    if (!pawn) return;
    Color c = pawn->color;
    // The pawn leaves the board, then the new piece (a captured one, or the spare queen) takes
    // its place.
    vec3 slot = board_.nextCaptureSlot(c);
    vec3 sqPos = board_.squareBase(sq);
    tasks.push_back(task(anim::TaskType::Reach, pawnId));
    tasks.push_back(task(anim::TaskType::Lift, pawnId, vec3(0), 0.03f));
    tasks.push_back(task(anim::TaskType::Carry, pawnId, slot));
    tasks.push_back(task(anim::TaskType::Place, pawnId, slot));
    dest_[pawnId].push_back({NoSquare, slot, true});
    int spareId = board_.takeSpare(newType, c);
    PieceObject* spare = board_.byId(spareId);
    vec3 target = jitteredSquare(sq);
    tasks.push_back(task(anim::TaskType::Reach, spareId));
    tasks.push_back(task(anim::TaskType::Lift, spareId, vec3(0), carryHeight(spare->basePos, sqPos, spareId, pawnId)));
    tasks.push_back(task(anim::TaskType::Carry, spareId, target));
    tasks.push_back(task(anim::TaskType::Place, spareId, target));
    dest_[spareId].push_back({sq, target, false});
}

// =============================================================================================
// Animation events
// =============================================================================================

void GameScene::handleEvents(int seat, std::vector<anim::Event>& events) {
    for (const anim::Event& e : events) {
        switch (e.type) {
        case anim::EventType::PieceGripped:
        case anim::EventType::CapturedGripped: {
            PieceObject* p = board_.byId(e.pieceId);
            if (!p) break;
            p->held = true;
            if (e.type == anim::EventType::CapturedGripped) p->square = NoSquare;
            audio::play(audio::Sfx::PiecePickup, p->transform.c[3].xyz(), e.type == anim::EventType::PieceGripped ? 0.8f : 0.5f);
            break;
        }
        case anim::EventType::PieceReleased:
        case anim::EventType::CapturedReleased: {
            PieceObject* p = board_.byId(e.pieceId);
            if (!p) break;
            Destination d;
            auto it = dest_.find(e.pieceId);
            if (it != dest_.end() && !it->second.empty()) {
                d = it->second.front();
                it->second.erase(it->second.begin());
                if (it->second.empty()) dest_.erase(it);
            } else {
                d.pos = e.position;
                d.captured = e.type == anim::EventType::CapturedReleased;
            }
            float heldYaw = yawOf(p->transform);
            if (d.captured) {
                board_.setCaptured(p->id, d.pos);
                p->yaw = heldYaw;
            } else if (d.square != NoSquare) {
                board_.setOnSquare(p->id, d.square);
                p->basePos = d.pos;
                // Keep the orientation the hand gave it, close to facing the opponent.
                float def = board_.defaultYaw(p->color);
                p->yaw = def + clamp(wrapAngle(heldYaw - def), -0.14f, 0.14f);
            } else {
                p->held = false;
                p->basePos = d.pos;
                p->yaw = heldYaw;
            }
            p->transform = translate(p->basePos) * rotateY(p->yaw);
            audio::play(e.type == anim::EventType::CapturedReleased ? audio::Sfx::Capture : audio::Sfx::PiecePlace, p->basePos,
                        0.9f);
            break;
        }
        case anim::EventType::ClockPressed: onClockPressed(seat); break;
        case anim::EventType::HandshakeClasp:
            if (seat == 0) audio::play(audio::Sfx::Handshake, vec3(0, layout::BOARD_TOP_Y + 0.22f, 0), 0.9f);
            break;
        default: break;
        }
    }
}

void GameScene::onClockPressed(int seat) {
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    audio::play(audio::Sfx::ClockPress, world_.clockPressPoint(half), 1.0f);
    leverTarget_ = half == 1 ? 1.0f : -1.0f;
    if (state_ != State::Playing) return;
    Color mover = colorOfSeat(seat);
    if (!(turn_ == Turn::HumanPressing || turn_ == Turn::AiMoving) || game_.position().sideToMove() != mover) return;
    chess::Arbiter::Verdict v = arbiter_.clockPressed(game_, clock_.timeControl());
    if (v.legal || v.moveStands) {
        clock_.press(mover);
        if (v.moveStands) {
            // Art. 7.5.2: pawn left unpromoted: penalised, the move stands with a queen.
            ui::notify(v.message, 6.0f);
            clock_.addTime(opposite(mover), v.opponentBonusMs);
        }
        game_.play(v.move);
        if (v.moveStands) board_.syncTo(game_.position());
        if (game_.status() != GameStatus::Ongoing) {
            endGame();
            return;
        }
        if (game_.position().inCheck() && mover != humanColor_ && !demo_) {
            // No announcement: players don't say "check" in tournaments. The arbiter stays quiet.
        }
        beginTurn();
        return;
    }
    // Illegal move completed: the arbiter restores the position and applies the penalty.
    ui::notify(v.message.empty() ? std::string("Illegal move.") : v.message, 6.0f);
    if (v.forfeit) {
        game_.forfeitIllegal(mover);
        endGame();
        return;
    }
    // The clock was not switched: the offender's time keeps running while the position is
    // restored.
    clock_.addTime(opposite(mover), v.opponentBonusMs);
    board_.syncTo(game_.position());
    leverTarget_ = -leverTarget_;
    beginTurn();
}

// =============================================================================================
// Camera, gaze, picking
// =============================================================================================

Ray GameScene::mouseRay() const {
    const plat::Input& in = plat::input();
    return camera_.screenRay(in.mouseX, in.mouseY, std::max(1, plat::width()), std::max(1, plat::height()));
}

int GameScene::pickPiece(const Ray& ray, float* tOut) const {
    int best = -1;
    float bestT = 1e30f;
    for (const PieceObject& p : board_.pieces()) {
        if (p.held || p.square == NoSquare) continue;
        float t = rayCylinderY(ray, p.basePos, layout::PIECE_BASE_RADIUS[p.type] * 1.12f, layout::PIECE_HEIGHT[p.type]);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = p.id;
        }
    }
    if (tOut) *tOut = bestT;
    return best;
}

Square GameScene::pickSquare(const Ray& ray) const {
    float t = rayPlane(ray, vec3(0, layout::BOARD_TOP_Y, 0), vec3(0, 1, 0));
    if (t < 0.0f) return NoSquare;
    vec3 p = ray.o + ray.d * t;
    int file = int(std::floor(p.x / layout::SQUARE_SIZE + 4.0f));
    int rank = int(std::floor(4.0f - p.z / layout::SQUARE_SIZE));
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return NoSquare;
    return makeSquare(file, rank);
}

void GameScene::updateCamera(float dt, bool firstPerson) {
    const plat::Input& in = plat::input();
    Settings& s = settings();
    camera_.fovY = kFov;
    camera_.nearZ = 0.02f;
    if (!firstPerson) {
        // Title screen: slow cinematic drift around the table.
        menuAngle_ += dt * 0.035f;
        float a = menuAngle_;
        vec3 eye(std::sin(a) * 2.7f, 1.45f + 0.08f * std::sin(time_ * 0.21f), std::cos(a) * 2.7f);
        camera_.position = eye;
        camera_.lookAt(vec3(0.0f, 0.92f, 0.0f));
        return;
    }
    bool canLook = state_ == State::Playing || state_ == State::Intro || state_ == State::Handshake || state_ == State::GameOver;
    bool uiBlocks = paused_ || gameOverShown_ || turn_ == Turn::HumanPromotion;
    if (canLook && !uiBlocks) {
        if (in.mouseDown[plat::MOUSE_RIGHT] && !dragging_ && !ui::wantsMouse()) {
            dragging_ = true;
            plat::setMouseCaptured(true);
        }
        if (in.mousePressed[plat::MOUSE_MIDDLE] || (in.keyPressed['C'] && !ui::wantsKeyboard())) lookYaw_ = lookPitch_ = 0.0f;
        if (!ui::wantsMouse()) lean_ = clamp(lean_ + in.wheel * 0.2f, 0.0f, 1.0f);
    }
    if (dragging_ && !in.mouseDown[plat::MOUSE_RIGHT]) {
        dragging_ = false;
        plat::setMouseCaptured(false);
    }
    if (dragging_) {
        float k = 0.0022f * s.mouseSensitivity;
        lookYaw_ -= in.mouseDX * k;
        lookPitch_ -= in.mouseDY * k * (s.invertLook ? -1.0f : 1.0f);
    }
    lookYaw_ = clamp(lookYaw_, -1.45f, 1.45f);
    lookPitch_ = clamp(lookPitch_, -1.1f - kBaseGazePitch, 0.75f - kBaseGazePitch);
    // The gaze drifts a little towards the cursor, like eyes following the hand.
    float cx = 0.0f, cy = 0.0f;
    if (!dragging_ && in.mouseInWindow && plat::width() > 0 && !ctx_->screenshotMode) {
        cx = clamp(in.mouseX / float(plat::width()) - 0.5f, -0.5f, 0.5f);
        cy = clamp(in.mouseY / float(plat::height()) - 0.5f, -0.5f, 0.5f);
    }
    float targetYaw = lookYaw_ - cx * 0.11f;
    float targetPitch = kBaseGazePitch + lookPitch_ - cy * 0.08f;
    float k = 1.0f - std::exp(-dt * 7.0f);
    gazeYaw_ += (targetYaw - gazeYaw_) * k;
    gazePitch_ += (targetPitch - gazePitch_) * k;
    leanSmooth_ += (lean_ - leanSmooth_) * (1.0f - std::exp(-dt * 5.0f));
    headYaw_ = clamp(gazeYaw_, -kHeadYawLimit, kHeadYawLimit);
    headPitch_ = clamp(gazePitch_, kHeadPitchDown, kHeadPitchUp);
    anim_[humanSeat()].setHeadOverride(true, headYaw_, headPitch_);
}

void GameScene::placeFirstPersonCamera() {
    mat4 e = anim_[humanSeat()].eyeCameraTransform();
    quat q = fromMat3(mat3(normalize(e.c[0].xyz()), normalize(e.c[1].xyz()), normalize(e.c[2].xyz())));
    float eyeYaw = clamp(gazeYaw_ - headYaw_, -kEyeLimit, kEyeLimit);
    float eyePitch = clamp(gazePitch_ - headPitch_, -kEyeLimit, kEyeLimit);
    q = normalize(q * axisAngle(vec3(0, 1, 0), eyeYaw) * axisAngle(vec3(1, 0, 0), eyePitch));
    vec3 fwd = rotate(q, vec3(0, 0, -1));
    vec3 flat = normalize(vec3(fwd.x, 0.0f, fwd.z) + vec3(1e-4f, 0, 0));
    camera_.position = e.c[3].xyz() + flat * (0.11f * leanSmooth_) + vec3(0, -0.05f * leanSmooth_, 0);
    camera_.orientation = q;
}

void GameScene::updateGaze(float dt) {
    if (state_ == State::Loading) return;
    gazeTimer_ -= dt;
    int seats[2] = {aiSeat(), humanSeat()};
    int count = (demo_ || state_ == State::Menu || state_ == State::FadeToGame) ? 2 : 1;
    for (int i = 0; i < count; ++i) {
        int seat = seats[i];
        int other = 1 - seat;
        vec3 face = anim_[other].eyeCameraTransform().c[3].xyz();
        vec3 target = vec3(0, layout::BOARD_TOP_Y, 0);
        if (state_ == State::Playing) {
            Color stm = game_.position().sideToMove();
            bool myTurn = seatOf(stm) == seat;
            if (myTurn && turn_ == Turn::AiMoving && aiMoveTo_ != NoSquare) {
                target = board_.squareBase(aiMoveTo_);
            } else if (!myTurn && touchedSq_ != NoSquare) {
                target = board_.squareBase(touchedSq_);
            } else {
                target = aiGazeTarget_;
            }
            if (!myTurn && glanceTime_ > 0.0f) target = face;
        } else if (state_ == State::Handshake || state_ == State::Intro || state_ == State::GameOver) {
            target = face;
        } else {
            target = aiGazeTarget_;
        }
        anim_[seat].lookAt(target, 1.0f);
    }
    glanceTime_ -= dt;
    if (gazeTimer_ <= 0.0f) {
        // New point of interest on the board every couple of seconds, and now and then a glance
        // at the opponent.
        int sq = rng_.rangeInt(8, 55);
        aiGazeTarget_ = board_.squareBase(Square(sq)) + vec3(0, 0.02f, 0);
        gazeTimer_ = rng_.range(1.2f, 3.2f);
        if (rng_.uniform() < 0.18f) glanceTime_ = rng_.range(0.8f, 1.6f);
    }
}

std::vector<Marker> GameScene::markers() const {
    std::vector<Marker> out;
    if (state_ != State::Playing || paused_) return out;
    if (turn_ == Turn::HumanIdle && hoverId_ >= 0) {
        const PieceObject* p = board_.pieces().size() > size_t(hoverId_) ? &board_.pieces()[size_t(hoverId_)] : nullptr;
        if (p && p->square != NoSquare) out.push_back({p->square, 0, 1.0f});
    }
    if (turn_ == Turn::HumanTouched && touchedSq_ != NoSquare) {
        out.push_back({touchedSq_, 1, 1.0f});
        if (settings().showLegalMoves) {
            const Position& pos = game_.position();
            bool seen[64] = {};
            for (const Move& mv : pos.legalMovesFrom(touchedSq_)) {
                if (seen[mv.to]) continue;
                seen[mv.to] = true;
                bool capture = !pos.at(mv.to).empty() || (mv.flags & MoveEnPassant);
                out.push_back({mv.to, capture ? 3 : 2, 1.0f});
            }
        }
    }
    return out;
}

ClockDisplay GameScene::clockDisplay() const {
    ClockDisplay d;
    int hw = world_.clockHalfForSeat(1.0f), hb = 1 - hw;
    d.ms[hw] = clock_.remainingMs(White);
    d.ms[hb] = clock_.remainingMs(Black);
    d.running = clock_.isRunning() ? (clock_.running() == White ? hw : hb) : -1;
    d.flagged[hw] = clock_.flagged(White);
    d.flagged[hb] = clock_.flagged(Black);
    d.unlimited = clock_.timeControl().unlimited;
    d.paused = paused_ && state_ == State::Playing;
    d.leverSide = leverSide_;
    return d;
}

// =============================================================================================
// Rendering
// =============================================================================================

void GameScene::render(AppContext& ctx, float dt) {
    render::Renderer& r = *ctx.renderer;
    bool firstPerson = !(state_ == State::Menu || state_ == State::Loading || state_ == State::FadeToGame);
    if (firstPerson) placeFirstPersonCamera();

    render::Camera cam = camera_;
    bool hideOwnHead = firstPerson;
    if (!viewOverride_.empty()) {
        hideOwnHead = false;
        if (viewOverride_ == "side") {
            cam.position = vec3(1.35f, 1.28f, 0.05f);
            cam.lookAt(vec3(0.0f, 0.86f, 0.0f));
        } else if (viewOverride_ == "board") {
            cam.position = vec3(0.0f, 1.55f, 0.35f);
            cam.lookAt(vec3(0.0f, layout::BOARD_TOP_Y, 0.0f));
        } else if (viewOverride_ == "hall") {
            cam.position = vec3(4.5f, 2.2f, 8.5f);
            cam.lookAt(vec3(-2.0f, 1.8f, 0.0f));
        } else if (viewOverride_ == "clock") {
            vec3 c = transformPoint(world_.clockTransform(), vec3(0, layout::CLOCK_HEIGHT * 0.5f, 0));
            cam.position = c + vec3(c.x > 0 ? -0.28f : 0.28f, 0.14f, 0.22f);
            cam.lookAt(c);
        } else if (viewOverride_ == "player") {
            hideOwnHead = true;
        }
    }
    camera_ = firstPerson && viewOverride_.empty() ? camera_ : camera_;
    render::Environment env = world_.environment(time_);
    r.fade = fade_;
    r.beginFrame(cam, env, dt);
    if (state_ != State::Loading) {
        world_.submitStatic(r);
        world_.submitPieces(r, board_);
        world_.submitClock(r, clockDisplay());
        for (int seat = 0; seat < 2; ++seat) {
            const mat4* g = anim_[seat].globals();
            world_.submitRobot(r, seat, g, hasPrevGlobals_[seat] ? prevGlobals_[seat] : nullptr,
                               hideOwnHead && seat == humanSeat());
            for (int b = 0; b < character::BoneCount; ++b) prevGlobals_[seat][b] = g[b];
            hasPrevGlobals_[seat] = true;
        }
        world_.submitMarkers(r, markers());
    }
    r.endFrame();
    audio::setListener(cam.position, cam.forward(), cam.up());
}

void GameScene::renderOverlay(AppContext&, float) {
    bool inGame = state_ == State::Playing || state_ == State::GameOver;
    ui::moveList(game_.sanMoves(), inGame && showMoveList_);
    ui::drawNotifications();
    ui::endFrame();
}

SCACELITH_SCENE("game", "Scacelith: the chess game", GameScene);

}  // namespace game
