// "coach" viewer scene: the table in the hall with the chess set and both robots seated, the coach
// wearing its "COACH" marking (World::setCoachSeat), and the Coach mode highlights: designated
// pieces (World::submitPieces with PieceHighlight) and squares and arrows on the board
// (World::submitCoachMarks). Orbit camera (right-drag / wheel) unless a first-person view is chosen.
//
// Options:
//   --view player|chest|side|board|marks|coords   camera preset: through the human's eyes at the
//                        board (default), through the human's eyes at the coach's chest, the chest
//                        from the side, the board from above, a close-up of the marks, a close-up
//                        of the human's near left corner of the border (files and ranks, --coords)
//   --human 0|1          the human's seat (0 = White at +Z, default); the coach sits opposite
//   --moves "e4 e5 ..."  moves played before (SAN, space separated; default a short opening)
//   --hl e4,d8           pieces highlighted, by square
//   --square e5,c6       squares marked
//   --arrow g1-f3,b8-d7  arrows; a knight move goes round its corner (long leg first), an explicit
//                        corner is written from-via-to (g1-g3-f3)
//   --demo               a white piece in the sun (c4), a black piece (c6), a light square with a
//                        piece (f7) and an empty dark one (d6), a straight arrow (c4-f7) and a
//                        knight's L (f3-g5); --hl, --square and --arrow add to it
//   --age S              age of every mark in seconds (default: time since the start, so the
//                        arrivals play; screenshots default to 3 s)
//   --strength S         strength of every highlight and mark (default 1)
//   --no-marking         no "COACH" on the coach
//   --coords             board coordinates (World::setBoardCoordinates)
//   --sun x,y,z          direction towards the sun (default: the hall's late morning sun)
//   --yaw Y --pitch P --dist D --fov F --target x,y,z   orbit camera overrides (radians / m / deg)
#include "layout.h"
#include "physical_board.h"
#include "world.h"
#include "../anim/animator.h"
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../character/skeleton.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../render/post/postfx.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace m;

namespace {

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep || c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

class CoachViewerScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        ctx_ = &ctx;
        while (!world_.loadStep(true)) {}
        world_.setupRenderer(*ctx.renderer);
        world_.setClockSide(true);
        human_ = std::atoi(ctx.argValue("--human", "0").c_str()) == 1 ? 1 : 0;
        coach_ = 1 - human_;
        if (!ctx.hasArg("--no-marking")) world_.setCoachSeat(coach_);
        world_.setBoardCoordinates(ctx.hasArg("--coords"));

        // Position.
        std::string moves = ctx.argValue("--moves", "e4 e5 Nf3 Nc6 Bc4 Bc5 c3 Nf6");
        for (const std::string& san : splitList(moves, ' ')) {
            chess::Move mv = game_.position().parseSAN(san);
            if (!game_.play(mv)) {
                LOGW("coach viewer: illegal move %s", san.c_str());
                break;
            }
        }
        board_.reset(true);
        board_.syncTo(game_.position());

        // Highlights and marks.
        std::string hl = ctx.argValue("--hl"), sq = ctx.argValue("--square"), ar = ctx.argValue("--arrow");
        if (ctx.hasArg("--demo")) {
            hl = "c4,c6" + (hl.empty() ? "" : "," + hl);
            sq = "f7,d6" + (sq.empty() ? "" : "," + sq);
            ar = "c4-f7,f3-g5" + (ar.empty() ? "" : "," + ar);
        }
        for (const std::string& s : splitList(hl, ',')) {
            chess::Square q = chess::parseSquare(s);
            const game::PieceObject* p = q == chess::NoSquare ? nullptr : board_.at(q);
            if (p) highlights_.push_back({p->id, 1.0f});
            else LOGW("coach viewer: no piece on %s", s.c_str());
        }
        for (const std::string& s : splitList(sq, ',')) {
            game::CoachMark mk;
            mk.kind = game::CoachMark::Square;
            mk.sq = chess::parseSquare(s);
            if (mk.sq != chess::NoSquare) marks_.push_back(mk);
        }
        for (const std::string& a : splitList(ar, ',')) {
            std::vector<std::string> sqs = splitList(a, '-');
            if (sqs.size() < 2) continue;
            game::CoachMark mk;
            mk.kind = game::CoachMark::Arrow;
            mk.from = chess::parseSquare(sqs.front());
            mk.to = chess::parseSquare(sqs.back());
            if (mk.from == chess::NoSquare || mk.to == chess::NoSquare) continue;
            if (sqs.size() == 3) {
                mk.via = chess::parseSquare(sqs[1]);
            } else {
                int df = chess::fileOf(mk.to) - chess::fileOf(mk.from), dr = chess::rankOf(mk.to) - chess::rankOf(mk.from);
                if (std::abs(df) * std::abs(dr) == 2)  // knight: the long leg first
                    mk.via = std::abs(dr) == 2 ? chess::makeSquare(chess::fileOf(mk.from), chess::rankOf(mk.to))
                                               : chess::makeSquare(chess::fileOf(mk.to), chess::rankOf(mk.from));
            }
            marks_.push_back(mk);
        }
        std::string v = ctx.argValue("--age");
        fixedAge_ = !v.empty() ? float(std::atof(v.c_str())) : (ctx.screenshotMode ? 3.0f : -1.0f);
        strength_ = float(std::atof(ctx.argValue("--strength", "1").c_str()));

        // Robots seated at rest.
        const character::Skeleton& sk = character::robotSkeleton();
        for (int seat = 0; seat < 2; ++seat) {
            float zs = seat == 0 ? 1.0f : -1.0f;
            anim_[seat].init(sk, vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs, character::Side::Right);
            anim_[seat].setRestHand(vec3(zs * 0.24f, layout::TABLE_TOP_Y, zs * 0.34f));
            anim_[seat].pieceTransform = [this](int id) {
                const game::PieceObject* p = board_.byId(id);
                return p ? p->transform : mat4();
            };
        }

        view_ = ctx.argValue("--view", "player");
        setupCamera();
        return true;
    }

    void setupCamera() {
        float zs = human_ == 0 ? 1.0f : -1.0f;
        cam_.fovY = 40.0f * DEG;
        cam_.yaw = human_ == 0 ? 0.0f : PI;
        if (view_ == "side") {
            cam_.target = vec3(0.0f, 0.96f, -zs * 0.48f);
            cam_.distance = 0.9f;
            cam_.yaw += 0.75f;
            cam_.pitch = 0.12f;
        } else if (view_ == "board") {
            cam_.target = vec3(0.0f, layout::BOARD_TOP_Y, 0.0f);
            cam_.distance = 0.9f;
            cam_.pitch = 1.2f;
        } else if (view_ == "marks") {
            cam_.target = vec3(0.04f, layout::BOARD_TOP_Y, -zs * 0.02f);
            cam_.distance = 0.45f;
            cam_.pitch = 0.75f;
        } else if (view_ == "coords") {
            // The human's near left corner (a1 for White, h8 for Black), seen from over his left hand.
            cam_.target = vec3(-zs * 0.15f, layout::BOARD_TOP_Y, zs * 0.17f);
            cam_.distance = 0.42f;
            cam_.yaw -= 0.55f;
            cam_.pitch = 0.62f;
        } else {
            cam_.target = vec3(0.0f, 0.9f, 0.0f);
            cam_.distance = 1.4f;
            cam_.pitch = 0.4f;
        }
        std::string s;
        if (!(s = ctx_->argValue("--yaw")).empty()) cam_.yaw = float(std::atof(s.c_str()));
        if (!(s = ctx_->argValue("--pitch")).empty()) cam_.pitch = float(std::atof(s.c_str()));
        if (!(s = ctx_->argValue("--dist")).empty()) cam_.distance = float(std::atof(s.c_str()));
        if (!(s = ctx_->argValue("--fov")).empty()) cam_.fovY = float(std::atof(s.c_str())) * DEG;
        if (!(s = ctx_->argValue("--target")).empty()) {
            vec3 t;
            if (std::sscanf(s.c_str(), "%f,%f,%f", &t.x, &t.y, &t.z) == 3) cam_.target = t;
        }
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime + frameTime_ : time_ + dt;
        frameTime_ += dt;
        cam_.update(plat::input());
        std::vector<anim::Event> events;
        for (int seat = 0; seat < 2; ++seat) {
            // Both look at the board centre.
            anim_[seat].lookAt(vec3(0.0f, layout::BOARD_TOP_Y, 0.0f));
            anim_[seat].update(dt, events);
        }
        return !plat::input().keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env = world_.environment(time_);
        vec3 sun;
        std::string sunArg = ctx.argValue("--sun");
        if (!sunArg.empty() && std::sscanf(sunArg.c_str(), "%f,%f,%f", &sun.x, &sun.y, &sun.z) == 3) env.sunDirection = normalize(sun);
        render::Camera cam = cam_.camera();
        int headless = -1;
        if (view_ == "player" || view_ == "chest") {
            // Through the human's eyes (the human's own head is not drawn), 52 degrees as in game.
            float zs = human_ == 0 ? 1.0f : -1.0f;
            cam.position = vec3(0.0f, layout::EYE_HEIGHT, zs * 0.55f);
            cam.fovY = 52.0f * DEG;
            cam.nearZ = 0.02f;
            cam.lookAt(view_ == "chest" ? vec3(0.0f, 0.97f, -zs * 0.50f) : vec3(0.0f, layout::BOARD_TOP_Y, -zs * 0.03f));
            std::string s;
            if (!(s = ctx.argValue("--fov")).empty()) cam.fovY = float(std::atof(s.c_str())) * DEG;
            headless = human_;
        }
        vec3 focus = view_ == "player" || view_ == "chest" ? (view_ == "chest" ? vec3(0.0f, 0.97f, (human_ == 0 ? -0.5f : 0.5f))
                                                                              : vec3(0.0f, layout::BOARD_TOP_Y, 0.0f))
                                                          : cam_.target;
        r.post().settings.dofFocusDistance = length(cam.position - focus);
        r.beginFrame(cam, env, dt);
        world_.submitStatic(r);
        board_.beginFrame();
        board_.updateRestingTransforms();
        std::vector<game::PieceHighlight> hl = highlights_;
        for (auto& h : hl) h.strength = strength_;
        world_.submitPieces(r, board_, &hl);
        game::ClockDisplay cd;
        cd.unlimited = true;
        world_.submitClock(r, cd);
        for (int seat = 0; seat < 2; ++seat) {
            const mat4* g = anim_[seat].globals();
            world_.submitRobot(r, seat, g, hasPrev_ ? prev_[seat] : nullptr, seat == headless);
            for (int b = 0; b < character::BoneCount; ++b) prev_[seat][b] = g[b];
        }
        hasPrev_ = true;
        std::vector<game::CoachMark> marks = marks_;
        float age = fixedAge_ >= 0.0f ? fixedAge_ + frameTime_ : frameTime_;
        for (auto& mk : marks) {
            mk.age = age;
            mk.strength = strength_;
        }
        world_.submitCoachMarks(r, marks);
        r.endFrame();
    }

    void shutdown(AppContext&) override {}

private:
    AppContext* ctx_ = nullptr;
    game::World world_;
    game::PhysicalBoard board_;
    chess::Game game_;
    anim::Animator anim_[2];
    mat4 prev_[2][character::BoneCount];
    bool hasPrev_ = false;
    int human_ = 0, coach_ = 1;
    std::vector<game::PieceHighlight> highlights_;
    std::vector<game::CoachMark> marks_;
    OrbitCamera cam_;
    std::string view_;
    float time_ = 0.0f, frameTime_ = 0.0f, fixedAge_ = -1.0f, strength_ = 1.0f;
};

}  // namespace

SCACELITH_SCENE("coach", "Coach mode visuals: the COACH marking, piece highlights, square and arrow marks (--demo, --view)",
                CoachViewerScene);
