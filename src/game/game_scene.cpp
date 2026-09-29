#include "game_scene.h"
#include "../audio/audio.h"
#include "../character/skeleton.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../render/post/postfx.h"
#include "elo.h"
#include "../ui/ui_font.h"
#include "layout.h"
#include "scoresheet_layout.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace m;
using namespace chess;

#ifndef SCACELITH_VERSION_STRING
#define SCACELITH_VERSION_STRING "Scacelith 0.1"
#endif

namespace game {

namespace {

constexpr float kFadeOut = 0.8f;          // menu -> black
constexpr float kFadeIn = 1.6f;           // black -> seated at the table
constexpr float kBaseGazePitch = -0.62f;  // looking down at the board from the chair
constexpr float kFov = 52.0f * DEG;
constexpr float kHeadYawLimit = 70.0f * DEG;
constexpr float kHeadPitchDown = -45.0f * DEG, kHeadPitchUp = 30.0f * DEG;
constexpr float kEyeLimit = 18.0f * DEG;
constexpr float kGlanceFov = 24.0f * DEG;  // looking at one's own scoresheet (S): a closer look
constexpr float kGlanceTime = 0.45f;       // seconds to turn to the scoresheet and back

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

// Also the suffix of the "notify.touched.*" translation keys.
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

// Heavier pieces sound slightly lower.
float piecePitch(PieceType t) {
    static const float kPitch[7] = {1.0f, 1.05f, 1.0f, 1.0f, 0.98f, 0.96f, 0.94f};
    return kPitch[t];
}

float distPointSegment2D(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 1e-9f ? clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

bool parseVec3(const std::string& v, vec3& out) {
    std::vector<std::string> c = split(v, ',');
    if (c.size() != 3) return false;
    out = vec3(float(std::atof(c[0].c_str())), float(std::atof(c[1].c_str())), float(std::atof(c[2].c_str())));
    return true;
}

// The seat whose clock stands on its left plays (and presses the clock) with its left hand.
// White (+Z, facing -Z) has +X on its right, Black the opposite.
character::Side playHandFor(int seat, bool clockPosX) {
    bool clockOnRight = (seat == 0) == clockPosX;
    return clockOnRight ? character::Side::Right : character::Side::Left;
}

// The human's name and handwriting on the scoresheets (Options > Player; "Human" by default, written
// in the interface language).
std::string localPlayerName() {
    const std::string& n = settings().playerName;
    return n.empty() || n == "Human" ? std::string(i18n::tr("player.default_name")) : n;
}
int humanHandStyle() { return int(settings().handStyle); }

const char* sideKey(Color c) { return c == White ? "viewer.side.white" : "viewer.side.black"; }

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

bool GameScene::init(AppContext& ctx) {
    ctx_ = &ctx;
    rng_.seedWith(ctx.screenshotMode ? 20260927u : plat::randomSeed());
    startWatching_ = ctx.hasArg("--viewer") || ctx.hasArg("--demo");
    skipIntro_ = ctx.hasArg("--no-intro");
    handoverPreview_ = ctx.hasArg("--handover-preview");
    debugCamera_ = ctx.hasArg("--cam");
    if (ctx.hasArg("--mouse")) {
        std::vector<std::string> c = split(ctx.argValue("--mouse"), ',');
        if (c.size() == 2) {
            mouseOverride_ = true;
            mouseOverridePos_ = vec2(float(std::atof(c[0].c_str())), float(std::atof(c[1].c_str())));
        }
    }

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
    int presetCount = int(ai::presets().size());
    watch_.whitePreset = std::clamp(s.viewerWhitePreset, 0, presetCount - 1);
    watch_.blackPreset = std::clamp(s.viewerBlackPreset, 0, presetCount - 1);
    watch_.timeControl = s.viewerTimeControl;
    watch_.customBaseSeconds = s.viewerCustomBaseSeconds;
    watch_.customIncrementSeconds = s.viewerCustomIncrementSeconds;
    watch_.customDelaySeconds = s.viewerCustomDelaySeconds;
    hudVisible_ = s.viewerShowControls;
    // Command-line games (screenshots, tests).
    if (ctx.hasArg("--white-preset")) watch_.whitePreset = std::clamp(std::atoi(ctx.argValue("--white-preset").c_str()), 0, presetCount - 1);
    if (ctx.hasArg("--black-preset")) watch_.blackPreset = std::clamp(std::atoi(ctx.argValue("--black-preset").c_str()), 0, presetCount - 1);
    if (ctx.hasArg("--tc")) {
        int tc = std::atoi(ctx.argValue("--tc").c_str());
        setup_.timeControl = watch_.timeControl = std::clamp(tc, 0, int(timeControlPresets().size()) - 1);
    }

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
    ui::setResolutionList({{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}});
    ui::setVersionString(SCACELITH_VERSION_STRING);
    ui::setSoundCallback([](ui::Sound snd) {
        switch (snd) {
        case ui::Sound::Hover: audio::playUI(audio::Sfx::UIHover, 0.35f); break;
        case ui::Sound::Tick: audio::playUI(audio::Sfx::UIHover, 0.25f); break;
        default: audio::playUI(audio::Sfx::UIClick, 0.6f); break;
        }
    });
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
    if (!scorekeeper_.init(true)) LOGW("scoresheets unavailable");
    engineOk_ = engine_.start();
    if (!engineOk_) LOGW("Stockfish is unavailable: the opponent will play random legal moves");
    initAnimators();
    board_.reset(true);
    world_.setClockSide(true);
    clock_.setup(chosenTimeControl());
    if (ctx_->hasArg("--start") || startWatching_) {
        mode_ = startWatching_ ? GameMode::Watch : GameMode::Play;
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
        glance_ = !watching() && ctx_->hasArg("--glance");
    } else {
        enterMenu();
    }
}

void GameScene::initAnimators() {
    const character::Skeleton& sk = character::robotSkeleton();
    for (int seat = 0; seat < 2; ++seat) {
        float zs = seat == 0 ? 1.0f : -1.0f;  // White at +Z faces -Z
        // The hand on the clock side plays and presses the clock; the other one writes.
        seats_[seat].playHand = playHandFor(seat, world_.clockOnPositiveX());
        anim_[seat] = anim::Animator();
        anim_[seat].init(sk, vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs, seats_[seat].playHand);
        // The playing hand rests on the table beside the board, on the clock side (White's right
        // is +X, Black's is -X).
        float side = anim_[seat].playHand() == character::Side::Right ? zs : -zs;
        anim_[seat].setRestHand(vec3(side * 0.24f, layout::TABLE_TOP_Y, zs * 0.34f));
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
        // Exact obstacle heights for the hand paths: standing pieces only (not those in a hand).
        anim_[seat].pathObstacleTop = [this](vec3 from, vec3 to) {
            float top = layout::BOARD_TOP_Y;
            vec2 a(from.x, from.z), b(to.x, to.z);
            for (const PieceObject& p : board_.pieces()) {
                if (pieceInHand(p)) continue;
                float d = distPointSegment2D(vec2(p.basePos.x, p.basePos.z), a, b);
                if (d < layout::PIECE_BASE_RADIUS[p.type] + 0.02f) top = std::max(top, p.basePos.y + layout::PIECE_HEIGHT[p.type]);
            }
            return top;
        };
        anim_[seat].obstacleTopNear = [this](vec3 pt, float radius, int ignoreId) {
            float top = layout::BOARD_TOP_Y;
            for (const PieceObject& p : board_.pieces()) {
                if (p.id == ignoreId || pieceInHand(p)) continue;
                float d = length(vec2(p.basePos.x - pt.x, p.basePos.z - pt.z));
                if (d < radius + layout::PIECE_BASE_RADIUS[p.type]) top = std::max(top, p.basePos.y + layout::PIECE_HEIGHT[p.type]);
            }
            return top;
        };
        hasPrevGlobals_[seat] = false;
    }
}

bool GameScene::pieceInHand(const PieceObject& p) const {
    return p.held || anim_[0].holding(p.id) || anim_[1].holding(p.id);
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
    newScoresheets();
    for (auto& a : anim_) a.setHeadOverride(false);
    menuAngle_ = 0.9f;
    cameraCut_ = true;
    plat::setMouseCaptured(false);
    dragging_ = false;
    glance_ = false;
    glanceBlend_ = 0.0f;
    pressTouched_ = false;
    observerPlaced_ = false;
    followEyes_ = false;
    clockFrozen_ = false;
}

TimeControl GameScene::chosenTimeControl() const {
    const auto& presets = timeControlPresets();
    // Watching uses the choice of the Watch a Game page ([viewer] in the .ini).
    int index = watching() ? watch_.timeControl : setup_.timeControl;
    int base = watching() ? watch_.customBaseSeconds : setup_.customBaseSeconds;
    int inc = watching() ? watch_.customIncrementSeconds : setup_.customIncrementSeconds;
    int delay = watching() ? watch_.customDelaySeconds : setup_.customDelaySeconds;
    if (index >= 0 && index < int(presets.size())) return presets[size_t(index)];
    TimeControl tc;
    tc.unlimited = false;
    tc.baseMs = int64_t(std::max(10, base)) * 1000;
    tc.incrementMs = int64_t(std::max(0, inc)) * 1000;
    tc.delayMs = int64_t(std::max(0, delay)) * 1000;
    return tc;
}

ai::EngineSettings GameScene::engineSettingsFor(int preset) const {
    const auto& presets = ai::presets();
    int idx = std::clamp(preset, 0, int(presets.size()) - 1);
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
    ++round_;
    if (watching()) {
        humanColor_ = White;  // nobody: keeps the human-game helpers well defined
        LOGI("New game (watching): %s vs %s, %s", ai::presets()[size_t(watch_.whitePreset)].name,
             ai::presets()[size_t(watch_.blackPreset)].name, chosenTimeControl().label().c_str());
    } else {
        humanColor_ = s.nextColor < 0 ? (rng_.uniform() < 0.5f ? White : Black) : Color(s.nextColor & 1);
        std::string forced = ctx_->argValue("--human");
        if (forced == "white") humanColor_ = White;
        if (forced == "black") humanColor_ = Black;
        LOGI("New game: human plays %s, %s, difficulty %d", humanColor_ == White ? "White" : "Black",
             chosenTimeControl().label().c_str(), setup_.difficulty);
    }

    game_.reset();
    arbiter_.reset(game_);
    // At the human player's right hand; at White's right when watching.
    bool clockPosX = watching() || humanColor_ == White;
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
    for (int i = 0; i < 2; ++i) {
        lastEval_[i] = 0;
        hasEval_[i] = false;
        lastOfferPly_[i] = -1;
    }
    pendingOffer_ = -1;
    clockFrozen_ = false;
    gameOverShown_ = false;
    showMoveList_ = false;
    rated_ = eloCounted_ = false;
    eloBefore_ = eloAfter_ = s.playerElo;

    initAnimators();
    configureSeats();
    newScoresheets();
    // The players filled in their header before sitting down at the board, as in a tournament
    // round: the pens only record the moves.
    scorekeeper_.writeHeaderInstantly();
    if (engineOk_) {
        engine_.newGame();
        engine_.configure(seats_[seats_[0].human() ? 1 : 0].engine);
    }
    if (watching()) {
        anim_[0].setHeadOverride(false);
        anim_[1].setHeadOverride(false);
    } else {
        anim_[humanSeat()].setHeadOverride(true, 0.0f, kBaseGazePitch);
        anim_[aiSeat()].setHeadOverride(false);
    }
    lookYaw_ = lookPitch_ = 0.0f;
    gazeYaw_ = 0.0f;
    gazePitch_ = kBaseGazePitch;
    lean_ = leanSmooth_ = 0.0f;
    cameraCut_ = true;

    if (watching()) {
        // Observer: --cam / --look / --fov, else --viewpoint N, else beside the table. The jump
        // happens once the robots are posed (face viewpoints need their heads).
        if (!followEyes_) eyesSeat_ = -1;
        // "Watch again" keeps the observer where it is.
        if (observerPlaced_) {
            if (followEyes_) pendingViewpoint_ = 0;  // into the eyes of White, who moves first
        } else {
            observerPlaced_ = true;
            pendingCamArg_ = ctx_->hasArg("--cam") && round_ == 1;
            pendingViewpoint_ = handoverPreview_ ? 0 : 1;
            if (ctx_->hasArg("--viewpoint") && round_ == 1)
                pendingViewpoint_ = std::clamp(std::atoi(ctx_->argValue("--viewpoint").c_str()), 0, 9);
        }
        viewpointShown_ = -1;
    }

    std::string moves = ctx_->argValue("--moves");
    if (!moves.empty()) applyMovesInstantly(split(moves, ','));
}

void GameScene::configureSeats() {
    const Settings& s = settings();
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        if (!watching() && st.color == humanColor_) {
            st.controller = Controller::Human;
            st.name = localPlayerName();
            st.elo = s.playerElo;
            st.provisional = s.playerGames < elo::kProvisionalGames;
        } else {
            st.controller = Controller::Stockfish;
            st.name = "Stockfish";
            st.preset = watching() ? (i == 0 ? watch_.whitePreset : watch_.blackPreset) : setup_.difficulty;
            st.preset = std::clamp(st.preset, 0, int(ai::presets().size()) - 1);
            st.engine = engineSettingsFor(st.preset);
            st.elo = ai::presetElo(st.preset, st.engine);
            st.presetName = ai::presets()[size_t(st.preset)].name;
        }
    }
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
    // The moves are on the scoresheets already, as if the game had been adjourned and resumed.
    if (!game_.sanMoves().empty()) scorekeeper_.writeMovesInstantly(game_.sanMoves());
}

void GameScene::newScoresheets() {
    Scorekeeper::Player p[2];
    for (int i = 0; i < 2; ++i) {
        p[i].name = seats_[i].name;
        p[i].elo = seats_[i].elo;
        p[i].handStyle = handStyleOf(i);
        p[i].blueInk = seats_[i].human() || i == 0;  // Stockfish as Black writes in black
    }
    scorekeeper_.newGame(anim_, world_.clockOnPositiveX(), p, std::max(1, round_), scoresheetDate(ctx_->screenshotMode));
}

int GameScene::handStyleOf(int seat) const {
    // Every sheet is written in its owner's hand; the two players never share one.
    int human = std::clamp(humanHandStyle(), 0, int(ui::font::HAND_STYLE_COUNT) - 1);
    if (seats_[seat].human()) return human;
    int other = seats_[1 - seat].human() ? human : -1;
    int want = watching() ? (seat == 0 ? ui::font::HAND_MARCK : ui::font::HAND_BADSCRIPT) : ui::font::HAND_MARCK;
    if (want == other) want = (want + 1) % ui::font::HAND_STYLE_COUNT;
    return want;
}

void GameScene::startPlaying() {
    state_ = State::Playing;
    stateTime_ = 0.0f;
    if (!watching()) {
        // Colours alternate from one game to the next.
        settings().nextColor = int(opposite(humanColor_));
        settings().save();
    }
    if (game_.status() != GameStatus::Ongoing) {
        endGame();
        return;
    }
    clock_.start(game_.position().sideToMove());
    audio::playUI(audio::Sfx::GameStart, 0.6f);
    // Both players take their pen while White thinks.
    scorekeeper_.startRecording();
    beginTurn();
}

void GameScene::beginTurn() {
    Color stm = game_.position().sideToMove();
    touchedId_ = -1;
    touchedSq_ = placedTo_ = NoSquare;
    pressQueued_ = false;
    hoverId_ = -1;
    aimSq_ = NoSquare;
    aimLegal_ = false;
    pressTouched_ = false;
    if (isHumanSeat(seatOf(stm))) {
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
    LOGI("Game over: %s (%s)\n%s", resultText_.c_str(), reasonText_.c_str(), game_.pgn(seats_[0].name, seats_[1].name).c_str());
    pendingOffer_ = -1;
    clockFrozen_ = false;
    rateGame();
    // Both players write the result and lay their pen down before shaking hands.
    scorekeeper_.finishGame(resultText_);
    audio::playUI(audio::Sfx::GameEnd, 0.7f);
}

void GameScene::rateGame() {
    if (rated_ || watching() || game_.status() == GameStatus::Ongoing) return;
    rated_ = true;
    Settings& s = settings();
    eloBefore_ = eloAfter_ = s.playerElo;
    // As in FIDE rating, a game counts once both players have made a move (a game abandoned or
    // lost on time before that is not rated). Without Stockfish the opponent plays random moves.
    if (game_.moves().size() < 2 || !engineOk_) {
        LOGI("Elo: game not rated (%d plies%s)", int(game_.moves().size()), engineOk_ ? "" : ", no engine");
        return;
    }
    GameStatus st = game_.status();
    double score = st == GameStatus::Draw ? 0.5 : ((st == GameStatus::WhiteWins) == (humanColor_ == White) ? 1.0 : 0.0);
    elo::Record r;
    r.rating = s.playerElo;
    r.games = s.playerGames;
    r.wins = s.playerWins;
    r.draws = s.playerDraws;
    r.losses = s.playerLosses;
    r.peak = std::max(s.playerPeakElo, s.playerElo);
    int opponent = seats_[aiSeat()].elo;
    elo::Change c = elo::applyResult(r, opponent, score);
    s.playerElo = r.rating;
    s.playerGames = r.games;
    s.playerWins = r.wins;
    s.playerDraws = r.draws;
    s.playerLosses = r.losses;
    s.playerPeakElo = r.peak;
    s.save();
    eloCounted_ = true;
    eloBefore_ = c.before;
    eloAfter_ = c.after;
    LOGI("Elo: %d -> %d (%+d; score %.1f against %d, expected %.2f, K %d)", c.before, c.after, c.delta(), score, opponent,
         c.expected, c.k);
}

ui::GameOverExtras GameScene::gameOverExtras() const {
    ui::GameOverExtras x;
    if (watching()) {
        int moveNo = std::max(1, int(game_.moves().size() + 1) / 2);
        GameStatus st = game_.status();
        const char* key = st == GameStatus::WhiteWins   ? "viewer.gameover.white_wins"
                          : st == GameStatus::BlackWins ? "viewer.gameover.black_wins"
                                                        : "viewer.gameover.draw";
        x.line = i18n::trf(key, {std::to_string(moveNo)});
        auto label = [this](int i) { return ui::presetName(seats_[i].presetName) + " (" + std::to_string(seats_[i].elo) + ")"; };
        x.detail = i18n::trf("viewer.gameover.players", {label(0), label(1)});
        x.primaryLabel = i18n::tr("viewer.watch_again");
        return x;
    }
    if (eloCounted_) {
        int d = eloAfter_ - eloBefore_;
        std::string delta = (d > 0 ? "+" : d < 0 ? "\xE2\x88\x92" : "\xC2\xB1") + std::to_string(std::abs(d));
        x.detail = i18n::trf("elo.change", {std::to_string(eloBefore_), std::to_string(eloAfter_), i18n::ltr(delta)});
    } else if (rated_) {
        x.detail = i18n::trf("elo.unrated", {std::to_string(eloBefore_)});
    }
    return x;
}

void GameScene::applySettings(bool displayToo) {
    Settings& s = settings();
    if (ctx_ && ctx_->renderer) {
        ctx_->renderer->setSettings(s.renderSettings());
        PostSettings& ps = ctx_->renderer->post().settings;
        ps.exposureCompensation = s.brightness;
        // A seated player's eyes: gentle depth of field, only far objects soften.
        applyDofPreset(ps, s.depthOfField ? DofPreset::Subtle : DofPreset::Off);
        ps.dofFStop = 8.0f;
        ps.dofMaxRadius = 8.0f;
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

void GameScene::shutdown(AppContext& ctx) {
    // Closing the game in the middle of a rated game resigns it, like leaving to the menu
    // (screenshot runs excepted: they stop wherever the capture happens).
    if (!ctx.screenshotMode && !watching() && state_ == State::Playing && game_.status() == GameStatus::Ongoing) {
        game_.resign(humanColor_);
        rateGame();
    }
    if (osCursorHidden_) plat::setCursorVisible(true);
    osCursorHidden_ = false;
    engine_.shutdown();
    scorekeeper_.shutdown();
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
        ui::MenuAction a = ui::mainMenu(setup_, watch_);
        if (a == ui::MenuAction::StartWatching) {
            // The page saved the choice in the .ini ([viewer]); watch_ holds it.
            mode_ = GameMode::Watch;
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        } else if (a == ui::MenuAction::StartGame) {
            mode_ = GameMode::Play;
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
        if (watching()) {
            updateWatchInput();
            break;
        }
        // Esc opens the pause menu; once open, the menu handles Esc itself (back / resume).
        if (!paused_ && in.keyPressed[plat::KEY_ESCAPE] && turn_ != Turn::HumanPromotion) {
            paused_ = true;
            if (dragging_) {
                dragging_ = false;
                plat::setMouseCaptured(false);
            }
        }
        if (paused_) {
            bool canClaim = game_.canClaimThreefold() || game_.canClaimFiftyMove();
            bool canOffer = drawOfferPly_ != int(game_.moves().size());
            switch (ui::pauseMenu(canClaim, canOffer)) {
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
                else ui::notify(i18n::tr("notify.no_draw_claim"));
                break;
            case ui::MenuAction::BackToMainMenu:
                paused_ = false;
                if (game_.status() == GameStatus::Ongoing) game_.resign(humanColor_);
                rateGame();  // leaving resigns: the game is rated as a loss
                clock_.stop();
                state_ = State::FadeToMenu;
                stateTime_ = 0.0f;
                break;
            case ui::MenuAction::OptionsChanged: applySettings(true); break;
            default: break;
            }
        } else {
            if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
            if (isHumanTurn()) updateHumanInput();
        }
        break;
    }
    case State::Intro:
    case State::Handshake:
        if (watching()) updateWatchInput();
        break;
    case State::GameOver:
        if (watching()) updateWatchInput();
        if (stateTime_ > 1.2f && (endHandshakeDone_ || stateTime_ > 5.0f)) {
            gameOverShown_ = true;
            ui::MenuAction a = ui::gameOver(resultText_, reasonText_, playerWon_, isDraw_, int(game_.moves().size() + 1) / 2,
                                            gameOverExtras());
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
    if (!isHumanTurn()) {
        hoverId_ = -1;
        aimSq_ = NoSquare;
        clockHover_ = false;
    }
    // The game's pointer replaces the system arrow at the table (not over menus and cards).
    bool hideArrow = gameCursorShown() && !ui::wantsMouse();
    if (hideArrow != osCursorHidden_) {
        plat::setCursorVisible(!hideArrow);
        osCursorHidden_ = hideArrow;
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
        // The result is written and the pens laid down first (a writing hand may be the right one).
        if (!endHandshakeDone_ && stateTime_ > 0.8f && !anim_[0].busy() && !anim_[1].busy() &&
            !anim_[0].writingBusy() && !anim_[1].writingBusy()) {
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
    // Reading one's own scoresheet (S): the writing hand waits off the page meanwhile.
    for (int seat = 0; seat < 2; ++seat)
        scorekeeper_.setHandAside(seat, glance_ && !watching() && firstPerson && seat == humanSeat());
    if (!frozen && state_ != State::Loading) {
        for (int seat = 0; seat < 2; ++seat) {
            events_.clear();
            anim_[seat].update(dt, events_);
            handleEvents(seat, events_);
        }
        scorekeeper_.update();
    }
    // Pieces in a hand follow it; the others rest where they were put.
    for (PieceObject& p : board_.pieces()) {
        if (!p.held) continue;
        mat4 t;
        if (anim_[0].heldPieceTransform(p.id, t) || anim_[1].heldPieceTransform(p.id, t)) p.transform = t;
    }
    board_.updateRestingTransforms();

    // Viewer: initial camera, once the robots are posed (face viewpoints need their heads), then
    // the observer's frame.
    if (observerView()) {
        if (pendingCamArg_) {
            pendingCamArg_ = false;
            pendingViewpoint_ = -1;
            vec3 p, t;
            parseVec3(ctx_->argValue("--cam"), p);
            if (!parseVec3(ctx_->argValue("--look"), t)) t = vec3(0.0f, layout::BOARD_TOP_Y, 0.0f);
            float fov = float(std::atof(ctx_->argValue("--fov", "0").c_str()));
            observer_.setPose(CameraPose::looking(p, t, fov > 1.0f ? fov * DEG : kFov));
            cameraCut_ = true;
        }
        if (pendingViewpoint_ >= 0) {
            selectViewpoint(pendingViewpoint_, true);
            pendingViewpoint_ = -1;
        }
        updateObserver(dt);
    }
}

bool GameScene::isHumanTurn() const {
    return turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing ||
           turn_ == Turn::HumanPromotion || turn_ == Turn::HumanPlaced || turn_ == Turn::HumanPressing;
}

void GameScene::updatePlaying(float dt) {
    // The handover between two players (hot-seat) freezes the clock between a clock press and the
    // moment the next player can act: nothing counts and nobody acts.
    if (clockFrozen_) return;
    // Clock
    if (clock_.isRunning()) {
        clockAccumMs_ += double(dt) * 1000.0;
        int64_t ms = int64_t(clockAccumMs_);
        clockAccumMs_ -= double(ms);
        clock_.update(ms);
        Color r = clock_.running();
        if (!clock_.timeControl().unlimited && clock_.flagged(r)) {
            game_.flagFall(r);
            if (watching()) {
                ui::notify(i18n::tr(r == White ? "viewer.flag.white" : "viewer.flag.black"), 4.0f);
                endGame();
                return;
            }
            ui::notify(i18n::tr(r == humanColor_ ? "notify.flag_you" : "notify.flag_opponent"), 4.0f);
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
    PieceObject* p = pid >= 0 ? board_.byId(pid) : nullptr;
    bool ownPiece = p && p->color == humanColor_;
    hoverId_ = ownPiece && turn_ == Turn::HumanIdle ? pid : -1;
    float tClock = 1e30f;
    clockHover_ = world_.rayHitsClock(ray, &tClock) && tClock < tPiece;
    // While a piece is in hand the pointer designates a square (shown on the board, see markers()).
    bool castling = false;
    aimSq_ = turn_ == Turn::HumanTouched && !clockHover_ ? aimSquare(ray, &castling) : NoSquare;
    aimLegal_ = aimSq_ != NoSquare && legalDestination(aimSq_);
    // A touched piece without a legal move may be let go: pointing at another of your pieces then
    // offers it instead of a square.
    bool canSwitch = turn_ == Turn::HumanTouched && ownPiece && pid != touchedId_ && !castling &&
                     !arbiter_.touchedHasLegalMove(game_);
    if (canSwitch) {
        aimSq_ = NoSquare;
        hoverId_ = pid;
    }

    if (in.keyPressed[plat::KEY_SPACE] && !ui::wantsKeyboard()) {
        humanPressClock();
        return;
    }
    // Drag and drop: the piece touched by this press goes to the square where the button is
    // released (a release on its own square keeps it in hand, a click then chooses the square).
    if (pressTouched_ && !in.mouseDown[plat::MOUSE_LEFT]) {
        pressTouched_ = false;
        float moved = length(cursorPixels() - pressPos_);
        if (in.mouseReleased[plat::MOUSE_LEFT] && turn_ == Turn::HumanTouched && !dragging_ && !ui::wantsMouse() &&
            moved > 0.012f * float(std::max(1, plat::height())) && aimSq_ != NoSquare && aimSq_ != touchedSq_) {
            const PieceObject* occupant = board_.at(aimSq_);
            if (castling || !occupant || occupant->color != humanColor_) humanPlace(aimSq_);
        }
        return;
    }
    if (!in.mousePressed[plat::MOUSE_LEFT] || ui::wantsMouse() || dragging_) return;

    if (clockHover_) {
        humanPressClock();
        return;
    }

    switch (turn_) {
    case Turn::HumanIdle:
        if (ownPiece) {
            humanTouch(p->id);
            if (turn_ == Turn::HumanTouched) {
                pressTouched_ = true;
                pressPos_ = cursorPixels();
            }
        }
        break;
    case Turn::HumanTouched: {
        PieceObject* touched = board_.byId(touchedId_);
        if (canSwitch) {
            humanRelease();
            humanTouch(p->id);
            break;
        }
        if (castling) {
            humanPlace(aimSq_);
            break;
        }
        const PieceObject* occupant = aimSq_ != NoSquare ? board_.at(aimSq_) : nullptr;
        bool ownSquare = occupant && occupant->color == humanColor_ && occupant->id != touchedId_;
        if (aimSq_ == touchedSq_) {
            if (!arbiter_.touchedHasLegalMove(game_)) {
                humanRelease();
            } else if (touched) {
                ui::notify(i18n::tr(std::string("notify.touched.") + pieceName(touched->type)), 3.0f);
            }
        } else if (ownSquare || (aimSq_ == NoSquare && ownPiece)) {
            if (touched) ui::notify(i18n::tr(std::string("notify.touched.") + pieceName(touched->type)), 3.0f);
        } else if (aimSq_ != NoSquare) {
            humanPlace(aimSq_);
        }
        break;
    }
    case Turn::HumanPlaced: ui::notify(i18n::tr("notify.press_clock"), 2.5f); break;
    default: break;
    }
}

bool GameScene::legalDestination(Square to) const {
    const PieceObject* mover = board_.byId(touchedId_);
    if (!mover || touchedSq_ == NoSquare || to == NoSquare) return false;
    bool promo = mover->type == Pawn && (rankOf(to) == 7 || rankOf(to) == 0);
    return game_.position().findLegal(touchedSq_, to, promo ? Queen : NoPiece).valid();
}

Square GameScene::aimSquare(const Ray& ray, bool* castling) const {
    if (castling) *castling = false;
    if (touchedSq_ == NoSquare) return NoSquare;
    const PieceObject* touched = board_.byId(touchedId_);
    Square under = pickSquare(ray);
    float tPiece = 1e30f;
    int pid = pickPiece(ray, &tPiece);
    const PieceObject* p = pid >= 0 && pid != touchedId_ ? board_.byId(pid) : nullptr;
    if (touched) {
        // Pointing at the piece in hand (gripped on its square) designates its own square.
        float t = rayCylinderY(ray, touched->basePos, layout::PIECE_BASE_RADIUS[touched->type] * 1.12f,
                               layout::PIECE_HEIGHT[touched->type]);
        if (t >= 0.0f && (!p || t < tPiece)) return touchedSq_;
    }
    if (p && p->color == humanColor_) {
        // Castling by pointing at the rook once the king is in hand.
        if (touched && touched->type == King && p->type == Rook && rankOf(p->square) == rankOf(touchedSq_)) {
            Square to = makeSquare(fileOf(p->square) > fileOf(touchedSq_) ? 6 : 2, rankOf(touchedSq_));
            if (game_.position().findLegal(touchedSq_, to).valid()) {
                if (castling) *castling = true;
                return to;
            }
        }
        // A piece never goes onto one of its own side: look through them at the square behind,
        // which they often hide from a seated player.
        return under;
    }
    if (p && under != NoSquare && under != p->square && settings().showLegalMoves) {
        // An opposing piece stands in front of the square under the pointer: take whichever of
        // the two the touched piece can go to (hints shown only, else this would tell).
        if (!legalDestination(p->square) && legalDestination(under)) return under;
    }
    return p ? p->square : under;
}

void GameScene::humanTouch(int pieceId) {
    PieceObject* p = board_.byId(pieceId);
    if (!p || p->square == NoSquare) return;
    if (!arbiter_.touch(game_, p->square)) {
        Square committed = arbiter_.touchedSquare();
        ui::notify(i18n::trf("notify.touched_square", {squareName(committed)}), 3.0f);
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
        ui::notify(i18n::tr("notify.illegal"), 2.0f);
        return;
    }
    if (!arbiter_.place(game_, to, NoPiece)) {
        ui::notify(i18n::tr("notify.cannot_move"), 2.0f);
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
        ui::notify(i18n::tr("notify.move_first"), 2.0f);
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
        ui::notify(i18n::tr("notify.draw_already_offered"), 2.5f);
        return;
    }
    drawOfferPly_ = ply;
    bool accept = engineOk_ ? engine_.acceptsDraw(lastAiEval_, ply) : false;
    if (accept) {
        ui::notify(i18n::tr("notify.draw_accepted"), 3.0f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify(i18n::tr("notify.draw_declined"), 3.0f);
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
    // Arm movement + clock press of a typical move (anim::Timing), spent on the AI's clock.
    // Stockfish deducts the overhead of its next 52 moves from the time left (timeman.cpp), which
    // left it no time at all below 78 s (depth-1 moves in bullet and time trouble): scaled down
    // so that about half of the time stays for the search. The clock itself is charged the
    // humanised thinking time, not the search time.
    int64_t budget = clock_.remainingMs(game_.position().sideToMove()) + int64_t(tc.incrementMs) * 49;
    ci.moveOverheadMs = int(std::clamp<int64_t>(budget / 104, 10, 1500));
    return ci;
}

void GameScene::updateAi(float dt) {
    Color side = game_.position().sideToMove();
    int seat = seatOf(side);
    const Position& pos = game_.position();
    if (!aiRequested_) {
        if (engineOk_) {
            // One Stockfish for both sides when watching: each search uses its side's settings
            // (the engine clears its hash when they differ from the previous search's).
            engine_.configure(seats_[seat].engine);
            engine_.requestMove(game_.uciMoves(), clockInfo());
        }
        aiElapsed_ = 0.0f;
        aiThinkMs_ = -1;
        aiRequested_ = true;
        aiHasMove_ = false;
        anim_[seat].setThinking(true);
        return;
    }
    aiElapsed_ += dt;
    if (!aiHasMove_) {
        if (engineOk_ && !engine_.moveReady()) return;
        int evalCp = 0;
        std::string uci = engineOk_ ? engine_.takeMove(&evalCp) : std::string();
        lastAiEval_ = evalCp;
        lastEval_[seat] = evalCp;
        hasEval_[seat] = engineOk_;
        aiMove_ = pos.parseUCI(uci);
        if (!aiMove_.valid()) {
            std::vector<Move> legal = pos.legalMoves();
            if (legal.empty()) return;  // game end is detected when the previous move was played
            if (engineOk_) LOGW("engine returned '%s', playing a random move", uci.c_str());
            aiMove_ = legal[size_t(rng_.rangeInt(0, int(legal.size()) - 1))];
        }
        // Human-like thinking time, counted from the request (the search ran concurrently).
        int legalCount = int(pos.legalMoves().size());
        aiThinkMs_ = engineOk_ ? engine_.thinkTimeMs(clockInfo(), int(game_.moves().size()), legalCount, pos.inCheck()) : 900;
        aiHasMove_ = true;
    }
    if (aiElapsed_ * 1000.0f < float(aiThinkMs_)) return;
    Move mv = aiMove_;
    int evalCp = lastAiEval_;

    // The opponent claims a draw by repetition / fifty moves when it is not better.
    if ((game_.canClaimThreefold() || game_.canClaimFiftyMove()) && engineOk_ &&
        engine_.acceptsDraw(evalCp, int(game_.moves().size()))) {
        game_.claimDraw();
        if (game_.status() != GameStatus::Ongoing) {
            if (watching()) {
                ui::notify(i18n::trf("viewer.claims_draw", {i18n::tr(sideKey(side))}), 3.0f);
                endGame();
                return;
            }
            ui::notify(i18n::tr("notify.draw_claimed"), 3.0f);
            endGame();
            return;
        }
    }
    // Between two AIs, a draw may be offered along with the move (FIDE 9.1.2: make the move, offer,
    // press the clock); the opponent answers once the clock is pressed.
    int ply = int(game_.moves().size());
    if (watching() && engineOk_ &&
        engine_.offersDraw(evalCp, ply, lastOfferPly_[seat] < 0 ? -1 : ply - lastOfferPly_[seat])) {
        lastOfferPly_[seat] = ply;
        pendingOffer_ = seat;
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
    // its place. The player sets their own pawn down in their half, in the row of the pieces
    // they captured (nextCaptureSlot places a colour near the player who captures it).
    vec3 slot = board_.nextCaptureSlot(c == White ? Black : White);
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
        scorekeeper_.onEvent(seat, e);
        switch (e.type) {
        case anim::EventType::PieceGripped:
        case anim::EventType::CapturedGripped: {
            PieceObject* p = board_.byId(e.pieceId);
            if (!p) break;
            p->held = true;
            if (e.type == anim::EventType::CapturedGripped) p->square = NoSquare;
            if (e.type == anim::EventType::PieceGripped)
                audio::play(audio::Sfx::PiecePickup, p->transform.c[3].xyz(), 0.8f, piecePitch(p->type));
            else
                audio::play(audio::Sfx::CaptureClick, p->transform.c[3].xyz(), 0.9f, piecePitch(p->type));
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
            bool onTable = d.captured || d.square == NoSquare;
            audio::play(onTable ? audio::Sfx::TablePlace : audio::Sfx::PiecePlace, p->basePos, 0.9f, piecePitch(p->type));
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
        // Both players record the move on their scoresheet (their writing hands, off the clock).
        scorekeeper_.recordMove(int(game_.moves().size()) - 1, game_.sanMoves().back());
        LOGI("move %d: %s (%s, clocks %lld / %lld ms)", int(game_.moves().size()), game_.sanMoves().back().c_str(),
             mover == White ? "White" : "Black", (long long)clock_.remainingMs(White), (long long)clock_.remainingMs(Black));
        if (v.moveStands) board_.syncTo(game_.position());
        if (game_.status() != GameStatus::Ongoing) {
            endGame();
            return;
        }
        // No announcement of checks: players don't say "check" in tournaments, the arbiter stays
        // quiet.
        if (pendingOffer_ == seat) {
            pendingOffer_ = -1;
            answerAiDrawOffer(seat);
            if (game_.status() != GameStatus::Ongoing) return;
        }
        beginTurn();
        // Watching through the players' eyes: the view flies to the next player (the hot-seat
        // handover; --handover-preview also freezes the clock until the camera has landed).
        if (watching() && followEyes_) {
            int next = 1 - seat;
            CameraPose to = eyePose(next);
            observer_.flyTo(to, CameraFlight::kHandoverDuration,
                            CameraFlight::handoverShape(observer_.pose(), to, vec3(0, layout::BOARD_TOP_Y, 0)));
            eyesSeat_ = next;
            eyesSmooth_ = to;
            if (handoverPreview_) setClockFrozen(true);
        }
        return;
    }
    // Illegal move completed: the arbiter restores the position and applies the penalty.
    ui::notify(v.message.empty() ? std::string(i18n::tr("notify.illegal")) : v.message, 6.0f);
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

void GameScene::answerAiDrawOffer(int offeringSeat) {
    // The other AI judges the offer on its own last evaluation (its point of view).
    int other = 1 - offeringSeat;
    int ply = int(game_.moves().size());
    bool accept = engineOk_ && hasEval_[other] && engine_.acceptsDraw(lastEval_[other], ply);
    std::string offering = i18n::tr(sideKey(colorOfSeat(offeringSeat)));
    std::string answering = i18n::tr(sideKey(colorOfSeat(other)));
    LOGI("%s offers a draw (%d cp), %s %s (%d cp)", offering.c_str(), lastEval_[offeringSeat], answering.c_str(),
         accept ? "accepts" : "declines", lastEval_[other]);
    if (accept) {
        ui::notify(i18n::trf("viewer.offer_accepted", {offering, answering}), 4.0f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify(i18n::trf("viewer.offer_declined", {offering, answering}), 3.0f);
    }
}

// =============================================================================================
// Camera, gaze, picking
// =============================================================================================

vec2 GameScene::cursorPixels() const {
    if (mouseOverride_)
        return vec2(mouseOverridePos_.x * float(std::max(1, plat::width())), mouseOverridePos_.y * float(std::max(1, plat::height())));
    const plat::Input& in = plat::input();
    return vec2(in.mouseX, in.mouseY);
}

Ray GameScene::mouseRay() const {
    vec2 c = cursorPixels();
    return camera_.screenRay(c.x, c.y, std::max(1, plat::width()), std::max(1, plat::height()));
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
    // The viewer's observer camera moves after the animation (simulate()): it can look through a
    // robot's eyes.
    if (observerView()) return;
    const plat::Input& in = plat::input();
    Settings& s = settings();
    camera_.fovY = kFov;
    camera_.nearZ = 0.02f;
    if (!firstPerson) glance_ = false;
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
        if (in.mousePressed[plat::MOUSE_MIDDLE] || (in.keyPressed['C'] && !ui::wantsKeyboard())) {
            lookYaw_ = lookPitch_ = 0.0f;
            glance_ = false;
        }
        if (!ui::wantsMouse()) lean_ = clamp(lean_ + in.wheel * 0.2f, 0.0f, 1.0f);
        // S: a look at your own scoresheet, out of sight on the table beside you, and back.
        if (!watching() && in.keyPressed['S'] && !ui::wantsKeyboard()) glance_ = !glance_;
    }
    if (dragging_ && glanceBlend_ > 0.0f) {
        // Looking around from the scoresheet starts from where the eyes are.
        lookYaw_ = gazeYaw_;
        lookPitch_ = gazePitch_ - kBaseGazePitch;
        glance_ = false;
        glanceBlend_ = 0.0f;
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
    // The gaze drifts a little towards the cursor, like eyes following the hand; not while a piece
    // is in hand, when the board must stay still under the pointer that aims at a square.
    float cx = 0.0f, cy = 0.0f;
    bool aiming = turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing;
    if (!dragging_ && !aiming && in.mouseInWindow && plat::width() > 0 && !ctx_->screenshotMode) {
        vec2 c = cursorPixels();
        cx = clamp(c.x / float(plat::width()) - 0.5f, -0.5f, 0.5f);
        cy = clamp(c.y / float(plat::height()) - 0.5f, -0.5f, 0.5f);
    }
    float targetYaw = lookYaw_ - cx * 0.11f;
    float targetPitch = kBaseGazePitch + lookPitch_ - cy * 0.08f;
    glanceBlend_ = clamp(glanceBlend_ + (glance_ ? dt : -dt) / kGlanceTime, 0.0f, 1.0f);
    if (glanceBlend_ > 0.0f) {
        // Eyes on the middle of the scoresheet, from where they are now (the head turns them).
        vec3 eye = anim_[humanSeat()].eyeCameraTransform().c[3].xyz();
        vec3 d = glanceTarget() - eye;
        float zs = humanSeat() == 0 ? 1.0f : -1.0f;  // the seat faces -Z * zs
        float yaw = std::atan2(-zs * d.x, -zs * d.z);
        float pitch = std::atan2(d.y, length(vec2(d.x, d.z)));
        float b = smootherstep(glanceBlend_);
        targetYaw = lerp(targetYaw, yaw, b);
        targetPitch = lerp(targetPitch, pitch, b);
        camera_.fovY = lerp(kFov, kGlanceFov, b);
    }
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
    if (glanceBlend_ > 0.0f) {
        // Reading the scoresheet beside you, the head tilts a little towards the lines.
        sheet::PadFrame f = sheet::padFrame(humanSeat(), world_.clockOnPositiveX());
        vec3 fw = rotate(q, vec3(0, 0, -1)), up = rotate(q, vec3(0, 1, 0)), rt = rotate(q, vec3(1, 0, 0));
        vec3 pageUp = -f.down - fw * dot(-f.down, fw);
        float roll = std::atan2(dot(pageUp, rt), std::max(1e-4f, dot(pageUp, up)));
        roll = clamp(roll * 0.45f, -28.0f * DEG, 28.0f * DEG) * smootherstep(glanceBlend_);
        q = normalize(q * axisAngle(vec3(0, 0, 1), -roll));
    }
    vec3 fwd = rotate(q, vec3(0, 0, -1));
    vec3 flat = normalize(vec3(fwd.x, 0.0f, fwd.z) + vec3(1e-4f, 0, 0));
    camera_.position = e.c[3].xyz() + flat * (0.11f * leanSmooth_) + vec3(0, -0.05f * leanSmooth_, 0);
    camera_.orientation = q;
}

void GameScene::updateGaze(float dt) {
    if (state_ == State::Loading) return;
    gazeTimer_ -= dt;
    // Every robot not driven by a first-person player looks around by itself: the AI in a human
    // game, both players when watching and on the title screen.
    bool menu = state_ == State::Menu || state_ == State::FadeToGame;
    for (int seat = 0; seat < 2; ++seat) {
        if (!menu && isHumanSeat(seat)) continue;
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
            } else if (!myTurn && turn_ == Turn::AiMoving && aiMoveTo_ != NoSquare) {
                target = board_.squareBase(aiMoveTo_);  // the other AI's move
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
    // The piece under the pointer that a click would take (Idle, or a switch when the touched
    // piece cannot move).
    if ((turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched) && hoverId_ >= 0) {
        const PieceObject* p = board_.byId(hoverId_);
        if (p && p->square != NoSquare) out.push_back({p->square, 0, 1.0f});
    }
    if (turn_ == Turn::HumanTouched && touchedSq_ != NoSquare) {
        out.push_back({touchedSq_, 1, 1.0f});
        bool hints = settings().showLegalMoves;
        if (hints) {
            const Position& pos = game_.position();
            bool seen[64] = {};
            for (const Move& mv : pos.legalMovesFrom(touchedSq_)) {
                if (seen[mv.to] || mv.to == aimSq_) continue;
                seen[mv.to] = true;
                bool capture = !pos.at(mv.to).empty() || (mv.flags & MoveEnPassant);
                out.push_back({mv.to, capture ? 3 : 2, 1.0f});
            }
        }
        // Where a click would put the piece. With hints, a square it cannot reach stays neutral;
        // without them every square looks the same (nothing tells a legal move).
        if (aimSq_ != NoSquare && aimSq_ != touchedSq_) out.push_back({aimSq_, hints && !aimLegal_ ? 5 : 4, 1.0f});
    }
    return out;
}

bool GameScene::gameCursorShown() const {
    if (!settings().gameCursor || watching() || paused_ || dragging_ || ui::optionsOpen()) return false;
    if (state_ != State::Playing && state_ != State::Intro && state_ != State::Handshake && state_ != State::GameOver) return false;
    if (turn_ == Turn::HumanPromotion) return false;
    return !(state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded());
}

ui::GameCursor GameScene::gameCursorKind() const {
    if (state_ != State::Playing || !isHumanTurn()) return ui::GameCursor::Waiting;
    switch (turn_) {
    case Turn::HumanIdle: return hoverId_ >= 0 ? ui::GameCursor::Piece : ui::GameCursor::Idle;
    case Turn::HumanTouched:
        if (hoverId_ >= 0) return ui::GameCursor::Piece;
        if (aimSq_ == NoSquare || aimSq_ == touchedSq_) return ui::GameCursor::Holding;
        return settings().showLegalMoves && !aimLegal_ ? ui::GameCursor::Holding : ui::GameCursor::Square;
    case Turn::HumanPlacing:
    case Turn::HumanPlaced: return clockHover_ ? ui::GameCursor::Clock : ui::GameCursor::Idle;
    default: return ui::GameCursor::Idle;
    }
}

vec3 GameScene::glanceTarget() const {
    sheet::PadFrame f = sheet::padFrame(humanSeat(), world_.clockOnPositiveX());
    return f.center + vec3(0.0f, layout::SCORESHEET_THICKNESS, 0.0f);
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
    bool observer = observerView();
    if (firstPerson && !observer) placeFirstPersonCamera();

    render::Camera cam = camera_;
    int headless = -1;  // the robot the camera is in: drawn without its head
    if (observer) {
        headless = headNearCamera(cam.position);
    } else if (firstPerson) {
        headless = humanSeat();
        vec3 p, t;
        if (debugCamera_ && parseVec3(ctx.argValue("--cam"), p)) {
            // Detached camera in a human game (screenshots): --cam x,y,z [--look x,y,z] [--fov deg].
            cam.position = p;
            cam.lookAt(parseVec3(ctx.argValue("--look"), t) ? t : vec3(0.0f, layout::BOARD_TOP_Y, 0.0f));
            float fov = float(std::atof(ctx.argValue("--fov", "0").c_str()));
            if (fov > 1.0f) cam.fovY = fov * DEG;
            headless = headNearCamera(cam.position);
        }
    }
    render::Environment env = world_.environment(time_);
    // Eyes focus where the player looks: the board / table under the centre of the view.
    {
        float target;
        if (observer || (firstPerson && headless != humanSeat())) {
            target = observerFocus(cam);
        } else {
            Ray centre{cam.position, cam.forward()};
            // The board, or the scoresheet on the table while the player looks at it (S).
            float planeY = lerp(layout::BOARD_TOP_Y, layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS, glanceBlend_);
            float t = rayPlane(centre, vec3(0, planeY, 0), vec3(0, 1, 0));
            target = (t > 0.0f && t < 3.0f) ? t : 2.5f;
            if (!firstPerson) target = length(cam.position - vec3(0, 0.9f, 0));
        }
        focusDistance_ = focusDistance_ <= 0.0f ? target : focusDistance_ + (target - focusDistance_) * (1.0f - std::exp(-dt * 6.0f));
        r.post().settings.dofFocusDistance = focusDistance_;
    }
    if (cameraCut_) {
        r.post().settings.resetHistory = true;
        cameraCut_ = false;
        for (auto& h : hasPrevGlobals_) h = false;
    }
    // Eyes adapt to the page when the player looks at their scoresheet (it lies in the body's
    // shadow, and the sunlit floor around it would keep the exposure low).
    r.post().settings.exposureCompensation = settings().brightness + 0.6f * smootherstep(glanceBlend_);
    r.fade = fade_;
    r.beginFrame(cam, env, dt);
    if (state_ != State::Loading) {
        world_.submitStatic(r);
        world_.submitPieces(r, board_);
        world_.submitClock(r, clockDisplay());
        // Through the player's eyes, the playing arm fades to a see-through ghost while a piece is
        // in hand, so the squares under it stay readable (the piece itself stays opaque).
        bool ghostArm = !watching() && state_ == State::Playing && headless == humanSeat() &&
                        (turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing || turn_ == Turn::HumanPromotion);
        armSeeThrough_ = clamp(armSeeThrough_ + (ghostArm ? dt : -dt) / 0.2f, 0.0f, 1.0f);
        for (int seat = 0; seat < 2; ++seat) {
            const mat4* g = anim_[seat].globals();
            world_.submitRobot(r, seat, g, hasPrevGlobals_[seat] ? prevGlobals_[seat] : nullptr, seat == headless,
                               !watching() && seat == humanSeat() ? armSeeThrough_ : 0.0f, seats_[seat].playHand);
            for (int b = 0; b < character::BoneCount; ++b) prevGlobals_[seat][b] = g[b];
            hasPrevGlobals_[seat] = true;
        }
        world_.submitMarkers(r, markers());
        scorekeeper_.submit(r);
    }
    r.endFrame();
    audio::setListener(cam.position, cam.forward(), cam.up());
}

void GameScene::renderOverlay(AppContext&, float) {
    bool inGame = state_ == State::Playing || state_ == State::GameOver;
    if (observerView() && !paused_ && state_ != State::FadeToGame && state_ != State::FadeToMenu) {
        ui::ViewerHud hud;
        hud.visible = hudVisible_ && !(gameOverShown_ && !ui::gameOverFolded());
        for (int i = 0; i < 2; ++i) {
            std::string& name = i == 0 ? hud.white : hud.black;
            name = i18n::trf("viewer.player", {ui::presetName(seats_[i].presetName), std::to_string(seats_[i].elo)});
        }
        hud.sideToMove = state_ == State::Playing ? seatOf(game_.position().sideToMove()) : -1;
        if (viewpointShown_ >= 0) hud.viewpoint = i18n::tr("viewer.view." + std::to_string(viewpointShown_));
        hud.viewpointAge = viewpointAge_;
        hud.speed = observer_.speed();
        hud.speedAge = speedAge_;
        ui::viewerHud(hud);
    }
    ui::moveList(game_.sanMoves(), inGame && showMoveList_);
    ui::drawNotifications();
    if (osCursorHidden_ && (mouseOverride_ || (plat::input().mouseInWindow && !ctx_->screenshotMode)))
        ui::gameCursor(cursorPixels(), gameCursorKind());
    ui::endFrame();
}

// =============================================================================================
// Viewer mode: the observer camera
// =============================================================================================

bool GameScene::observerView() const {
    // From the setup of a watched game until the menu (a "Watch again" fade keeps it).
    return watching() && observerPlaced_ && state_ != State::Loading && state_ != State::Menu;
}

void GameScene::updateWatchInput() {
    const plat::Input& in = plat::input();
    observerControls_ = ObserverCamera::Controls();
    // Esc: the viewer's pause menu (the game stops, the camera can still land).
    if (state_ == State::Playing && !paused_ && in.keyPressed[plat::KEY_ESCAPE] && !ui::wantsKeyboard()) paused_ = true;
    if (paused_) {
        if (dragging_) {
            dragging_ = false;
            plat::setMouseCaptured(false);
        }
        switch (ui::viewerPauseMenu()) {
        case ui::MenuAction::Resume: paused_ = false; break;
        case ui::MenuAction::BackToMainMenu:
            paused_ = false;
            clock_.stop();
            state_ = State::FadeToMenu;
            stateTime_ = 0.0f;
            break;
        case ui::MenuAction::OptionsChanged: applySettings(true); break;
        default: break;
        }
        return;
    }
    // The game over card has the keyboard until it is folded ("View the board").
    bool cardUp = state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded();
    bool keys = !cardUp && (state_ == State::GameOver || !ui::wantsKeyboard());
    // Right mouse button held: look around.
    if (in.mouseDown[plat::MOUSE_RIGHT] && !dragging_ && !ui::wantsMouse()) {
        dragging_ = true;
        plat::setMouseCaptured(true);
    }
    if (dragging_ && !in.mouseDown[plat::MOUSE_RIGHT]) {
        dragging_ = false;
        plat::setMouseCaptured(false);
    }
    observerControls_ = ObserverCamera::read(in, keys, dragging_);
    if (ui::wantsMouse() && !dragging_) observerControls_.wheel = 0.0f;
    if (!keys) return;
    for (int n = 0; n <= 9; ++n) {
        if (in.keyPressed[plat::KEY_0 + n]) selectViewpoint(n, false);
    }
    if (in.keyPressed['H']) {
        hudVisible_ = !hudVisible_;
        settings().viewerShowControls = hudVisible_;
        settings().save();
    }
    // (Tab is handled by the game over state itself.)
    if (state_ != State::GameOver && in.keyPressed[plat::KEY_TAB]) showMoveList_ = !showMoveList_;
}

void GameScene::updateObserver(float dt) {
    const Settings& s = settings();
    ObserverCamera::Controls c = observerControls_;
    observerControls_ = ObserverCamera::Controls();  // once per frame (warps simulate many steps)
    viewpointAge_ += dt;
    speedAge_ += dt;
    if (c.wheel != 0.0f) speedAge_ = 0.0f;
    if (followEyes_ && c.any()) {
        // The observer leaves the player's eyes and flies on from there.
        followEyes_ = false;
        if (clockFrozen_) setClockFrozen(false);
    }
    if (followEyes_) {
        // Steadied eyes: the robots' saccades and small head motions are smoothed out.
        CameraPose e = eyePose(eyesSeat_);
        float k = 1.0f - std::exp(-dt * 5.0f);
        eyesSmooth_.position = e.position;
        eyesSmooth_.yaw += angleDelta(eyesSmooth_.yaw, e.yaw) * k;
        eyesSmooth_.pitch += (e.pitch - eyesSmooth_.pitch) * k;
        eyesSmooth_.roll += angleDelta(eyesSmooth_.roll, e.roll) * k;
        eyesSmooth_.fovY = e.fovY;
        observer_.retarget(eyesSmooth_);
    }
    bool wasFlying = observer_.flying();
    observer_.update(dt, c, s.mouseSensitivity, s.invertLook);
    // Handover preview: the next player's clock starts when the camera is in its eyes.
    if (wasFlying && !observer_.flying() && clockFrozen_) setClockFrozen(false);
    const CameraPose& p = observer_.pose();
    camera_.position = p.position;
    camera_.orientation = p.orientation();
    camera_.fovY = p.fovY;
    camera_.nearZ = 0.02f;
}

CameraPose GameScene::viewpoint(int n) const {
    // The development viewpoints of the former --view option.
    const vec3 board(0.0f, layout::BOARD_TOP_Y, 0.0f);
    switch (n) {
    case 1: return CameraPose::looking(vec3(1.35f, 1.28f, 0.05f), vec3(0.0f, 0.86f, 0.0f), kFov);  // beside the table
    case 2: return CameraPose::looking(vec3(0.0f, 1.55f, 0.35f), board, kFov);                  // above the board
    case 3: return CameraPose::looking(vec3(4.5f, 2.2f, 8.5f), vec3(-2.0f, 1.8f, 0.0f), kFov);    // the hall
    case 4: {                                                                                    // the clock
        vec3 c = transformPoint(world_.clockTransform(), vec3(0, layout::CLOCK_HEIGHT * 0.5f, 0));
        return CameraPose::looking(c + vec3(c.x > 0 ? -0.28f : 0.28f, 0.14f, 0.22f), c, kFov);
    }
    case 5:
    case 6: {  // three-quarter portrait of White's / Black's head
        int seat = n - 5;
        vec3 eye = anim_[seat].eyeCameraTransform().c[3].xyz();
        float f = seat == 0 ? -1.0f : 1.0f;  // White (seat 0) faces -Z
        return CameraPose::looking(eye + vec3(0.24f * f, 0.03f, 0.52f * f), eye + vec3(0.0f, -0.06f, 0.0f), 34.0f * DEG);
    }
    case 7: return CameraPose::looking(vec3(1.75f, 1.32f, 0.0f), vec3(0.0f, 1.02f, 0.0f), 42.0f * DEG);  // the duel
    case 8: return CameraPose::looking(vec3(5.6f, 2.1f, 3.4f), vec3(-2.0f, 2.2f, -0.4f), 55.0f * DEG);   // windows
    case 9: return CameraPose::looking(vec3(-5.2f, 1.9f, -3.6f), vec3(2.5f, 2.0f, 0.8f), 55.0f * DEG);  // tapestries
    default: return eyePose(eyesSeat_ < 0 ? 0 : eyesSeat_);
    }
}

CameraPose GameScene::eyePose(int seat) const {
    mat4 e = anim_[seat & 1].eyeCameraTransform();
    quat q = fromMat3(mat3(normalize(e.c[0].xyz()), normalize(e.c[1].xyz()), normalize(e.c[2].xyz())));
    return CameraPose::fromOrientation(e.c[3].xyz(), q, kFov);
}

void GameScene::selectViewpoint(int n, bool jump) {
    n = std::clamp(n, 0, 9);
    viewpointShown_ = n;
    viewpointAge_ = 0.0f;
    CameraPose to;
    if (n == 0) {
        // Through the eyes of the player to move, following them from one player to the other.
        followEyes_ = true;
        eyesSeat_ = seatOf(game_.position().sideToMove());
        to = eyePose(eyesSeat_);
        eyesSmooth_ = to;
    } else {
        followEyes_ = false;
        if (clockFrozen_) setClockFrozen(false);
        to = viewpoint(n);
    }
    if (jump) {
        observer_.setPose(to);
        cameraCut_ = true;
    } else {
        observer_.flyTo(to);
    }
}

int GameScene::headNearCamera(vec3 p) const {
    for (int seat = 0; seat < 2; ++seat) {
        mat4 e = anim_[seat].eyeCameraTransform();
        vec3 centre = e.c[3].xyz() + normalize(e.c[2].xyz()) * 0.06f;  // +Z: behind the eyes
        if (length(p - centre) < 0.16f) return seat;
    }
    return -1;
}

float GameScene::observerFocus(const render::Camera& cam) const {
    // A head in the middle of the view (portraits), else the board under the centre of the view,
    // else the table.
    vec3 fwd = cam.forward();
    for (int seat = 0; seat < 2; ++seat) {
        vec3 d = anim_[seat].eyeCameraTransform().c[3].xyz() - cam.position;
        float dist = length(d);
        if (dist > 0.2f && dist < 2.5f && dot(d / dist, fwd) > std::cos(10.0f * DEG)) return dist;
    }
    Ray centre{cam.position, fwd};
    float t = rayPlane(centre, vec3(0, layout::BOARD_TOP_Y, 0), vec3(0, 1, 0));
    if (t > 0.0f && t < 3.0f) {
        vec3 hit = cam.position + fwd * t;
        if (std::abs(hit.x) < 0.9f && std::abs(hit.z) < 0.9f) return t;
    }
    return std::max(0.3f, length(cam.position - vec3(0, 1.0f, 0)));
}

SCACELITH_SCENE("game", "Scacelith: the chess game", GameScene);

}  // namespace game
