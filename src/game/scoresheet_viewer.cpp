// "scoresheet" viewer scene: both players' scoresheet pads on the table in the hall (board, pieces
// and clock around them), with demos of the handwriting reveal, the page turn and the header.
// Orbit camera (right-drag / wheel); the demos run on the scene clock (--time T freezes it).
//
// Options:
//   --demo write|turn|header|static    write: moves written one after the other, a pen floating
//                                      along the path (default); turn: a full page turned over the
//                                      top edge; header: names and Elo written on both sheets;
//                                      static: everything already written
//   --view close|pad|header|macro|pen|corner|stack|player|overview|table   camera preset (default pad)
//   --sheet 0|1        the pad the camera looks at (0 = White's, default)
//   --moves N          plies already written when the demo starts (write: 6, static: 40)
//   --s S              turn demo: fixed flip progress in [0,1]
//   --names latin|cyrillic|arabic|cjk|mixed   player names (default mixed: Latin + Cyrillic)
//   --style0 N --style1 N              hand styles of White's / Black's sheet (0 Caveat, 1 Marck
//                                      Script, 2 Bad Script; default 0 and 1)
//   --lang CODE        language of the printed form, the piece letters and the date
//   --clock-left       the clock on -X (the pads move to +X)
//   --sun x,y,z        direction towards the sun (default: the hall's late morning sun)
//   --yaw Y --pitch P --dist D --fov F --target x,y,z   orbit camera overrides (radians / m / deg)
#include "layout.h"
#include "physical_board.h"
#include "scorekeeper.h"
#include "scoresheet.h"
#include "world.h"
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../render/materials/material_library.h"
#include "../render/post/postfx.h"
#include "../ui/ui_font.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace m;

namespace {

// Kasparov - Topalov, Wijk aan Zee 1999 (enough moves for two pages).
const char* const kGameMoves[] = {
    "e4",   "d6",   "d4",   "Nf6",  "Nc3",  "g6",   "Be3",  "Bg7",  "Qd2",  "c6",   "f3",    "b5",   "Nge2", "Nbd7", "Bh6",
    "Bxh6", "Qxh6", "Bb7",  "a3",   "e5",   "O-O-O", "Qe7", "Kb1",  "a6",   "Nc1",  "O-O-O", "Nb3",  "exd4", "Rxd4", "c5",
    "Rd1",  "Nb6",  "g3",   "Kb8",  "Na5",  "Ba8",  "Bh3",  "d5",   "Qf4+", "Ka7",  "Rhe1",  "d4",   "Nd5",  "Nbxd5", "exd5",
    "Qd6",  "Rxd4", "cxd4", "Re7+", "Kb6",  "Qxd4+", "Kxa5", "b4+", "Ka4",  "Qc3",  "Qxd5",  "Ra7",  "Bb7",  "Rxb7", "Qc4",
    "Qxf6", "Kxa3", "Qxa6+", "Kxb4", "c3+", "Kxc3", "Qa1+", "Kd2",  "Qb2+", "Kd1",  "Bf1",   "Rd2",  "Rd7",  "Rxd7", "Bxc4",
    "bxc4", "Qxh8", "Rd3",  "Qa8",  "c3",   "Qa4+", "Ke1",  "f4",   "f5",   "Kc1",  "Rd2",   "Qa7"};

class ScoresheetScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        ctx_ = &ctx;
        materials::init();
        if (!ctx.argValue("--lang").empty()) i18n::setLanguage(ctx.argValue("--lang"));
        if (!ui::font::ready()) ui::font::init();
        while (!world_.loadStep()) {}
        world_.setupRenderer(*ctx.renderer);
        clockPosX_ = !ctx.hasArg("--clock-left");
        world_.setClockSide(clockPosX_);
        buildGame();

        demo_ = ctx.argValue("--demo", "write");
        view_ = ctx.argValue("--view", "pad");
        sheetIdx_ = std::atoi(ctx.argValue("--sheet", "0").c_str()) == 1 ? 1 : 0;
        fixedS_ = float(std::atof(ctx.argValue("--s", "-1").c_str()));
        baseMoves_ = std::atoi(ctx.argValue("--moves", demo_ == "static" ? "40" : (demo_ == "turn" ? "80" : "6")).c_str());
        baseMoves_ = std::max(0, std::min(baseMoves_, int(san_.size())));

        std::string names = ctx.argValue("--names", "mixed");
        header_.date = game::scoresheetDate(true);  // 28 September 2026, written the language's way
        header_.round = "3";
        header_.whiteElo = "1850";
        header_.blackElo = "2410";
        if (names == "latin") {
            header_.white = "Emma Lindqvist";
            header_.black = "Stockfish";
        } else if (names == "cyrillic") {
            header_.white = "\xD0\x9E\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\x9A\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBB\xD1\x8C";  // Олена Коваль
            header_.black = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD \xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2";  // Иван Петров
        } else if (names == "arabic") {
            header_.white = "\xD8\xA3\xD8\xAD\xD9\x85\xD8\xAF \xD9\x85\xD9\x86\xD8\xB5\xD9\x88\xD8\xB1";  // أحمد منصور
            header_.black = "Stockfish";
        } else if (names == "cjk") {
            header_.white = "\xE5\xB1\xB1\xE7\x94\xB0 \xE5\xA4\xAA\xE9\x83\x8E";  // 山田 太郎
            header_.black = "\xE7\x8E\x8B\xE8\x8A\xB3";                            // 王芳
        } else {
            header_.white = "Emma Lindqvist";
            header_.black = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD \xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2";  // Иван Петров
        }

        for (int s = 0; s < 2; ++s) {
            game::Scoresheet::Config c;
            c.owner = s;
            c.clockOnPositiveX = clockPosX_;
            c.handStyle = std::atoi(ctx.argValue(s == 0 ? "--style0" : "--style1", s == 0 ? "0" : "1").c_str());
            c.seed = 1234u + uint32_t(s) * 77u;
            c.letters = game::localizedPieceLetters();
            if (s == 1) c.inkColor = vec3(0.018f, 0.018f, 0.024f);  // black ballpoint
            sheets_[s].init(c);
        }
        restart();
        setupCamera();
        return true;
    }

    void buildGame() {
        chess::Game g;
        for (const char* s : kGameMoves) {
            chess::Move mv = g.position().parseSAN(s);
            if (!g.position().isLegal(mv) || !g.play(mv)) break;
        }
        // Keep the demo going past the recorded game with deterministic legal moves.
        uint32_t h = 7u;
        while (g.sanMoves().size() < 200 && !g.isOver()) {
            std::vector<chess::Move> legal = g.position().legalMoves();
            if (legal.empty()) break;
            h = hash32(h + 1u);
            g.play(legal[h % legal.size()]);
        }
        san_ = g.sanMoves();
        game_ = g;
        if (san_.size() < 87) LOGW("scoresheet viewer: only %d plies of the demo game parsed", int(san_.size()));
    }

    // Re-plays the demo from the beginning: header and the first plies written instantly.
    void restart() {
        for (auto& s : sheets_) {
            s.reset();
            if (demo_ != "header") s.writeHeaderInstant(header_);
            for (int p = 0; p < baseMoves_; ++p) s.writeMoveInstant(p, san_[size_t(p)]);
        }
        nextPly_ = baseMoves_;
        entryStart_ = 0.0f;
        writing_ = false;
        turnDone_ = false;
        headerStep_ = 0;
        path_.clear();
        syncBoard(nextPly_);
    }

    void syncBoard(int plies) {
        chess::Game g;
        for (int p = 0; p < plies && p < int(game_.moves().size()); ++p) g.play(game_.moves()[size_t(p)]);
        board_.reset(clockPosX_);
        board_.syncTo(g.position());
    }

    void setupCamera() {
        const game::sheet::PadFrame& f = sheets_[sheetIdx_].frame();
        float back = f.owner == 0 ? 0.0f : PI;
        auto page = [&](float x, float y, float h) { return f.padToWorld(game::sheet::pageToPad(x, y, h)); };
        cam_.fovY = 40.0f * DEG;
        cam_.yaw = back;
        if (view_ == "close") {
            cam_.target = page(60.0f, 80.0f, 5.0f);
            cam_.distance = 0.24f;
            cam_.pitch = 1.0f;
            cam_.yaw = back + 0.2f;
        } else if (view_ == "header") {
            cam_.target = page(72.0f, 36.0f, 5.0f);
            cam_.distance = 0.2f;
            cam_.pitch = 1.05f;
        } else if (view_ == "macro") {
            game::sheet::Rect r = game::sheet::cellRect(game::sheet::cellOf(std::max(0, nextPly_ - 1)));
            cam_.target = page(r.cx(), r.cy(), 5.0f);
            cam_.distance = 0.075f;
            cam_.pitch = 0.9f;
            cam_.fovY = 35.0f * DEG;
            cam_.yaw = back + 0.3f;
        } else if (view_ == "pen") {
            cam_.target = sheets_[sheetIdx_].penRestTransform().translation() +
                          f.up * 0.006f + normalize(transformPoint(sheets_[sheetIdx_].penRestTransform(), vec3(0, 0.05f, 0)) -
                                                    sheets_[sheetIdx_].penRestTransform().translation()) * 0.02f;
            cam_.distance = 0.14f;
            cam_.pitch = 0.55f;
            cam_.yaw = back - 0.5f * f.outerSign * (f.owner == 0 ? 1.0f : -1.0f);
        } else if (view_ == "corner") {
            cam_.target = page(74.0f, 40.0f, 30.0f);
            cam_.distance = 0.55f;
            cam_.pitch = 0.3f;
            // From the outer side, a little in front.
            float side = f.right.x * f.outerSign > 0.0f ? 1.0f : -1.0f;
            cam_.yaw = std::atan2(side * 1.0f, f.down.z * 0.5f);
        } else if (view_ == "stack") {
            cam_.target = page(game::sheet::outerEdgeX(f), 200.0f, 2.5f);
            cam_.distance = 0.1f;
            cam_.pitch = 0.3f;
            float side = f.right.x * f.outerSign > 0.0f ? 1.0f : -1.0f;
            cam_.yaw = std::atan2(side * 0.8f, f.down.z * 1.0f);
        } else if (view_ == "overview") {
            cam_.target = vec3(0.0f, layout::TABLE_TOP_Y, 0.0f);
            cam_.distance = 1.5f;
            cam_.pitch = 0.62f;
            cam_.yaw = 0.35f;
        } else if (view_ == "table") {
            cam_.target = vec3(f.center.x * 0.6f, layout::TABLE_TOP_Y, 0.0f);
            cam_.distance = 0.9f;
            cam_.pitch = 0.8f;
            cam_.yaw = 0.0f;
        } else {  // pad
            cam_.target = page(74.0f, 105.0f, 5.0f);
            cam_.distance = 0.42f;
            cam_.pitch = 1.05f;
            cam_.yaw = back + 0.15f;
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

    // ---- Demos ----
    // Moves written one after the other on both sheets (each owner writes both moves); the pen
    // floats along the path of the sheet being looked at.
    void runWrite(float t) {
        while (true) {
            if (!writing_) {
                if (nextPly_ >= int(san_.size()) || nextPly_ >= 160) {
                    restart();
                    t0_ = t;
                    return;
                }
                for (int s = 0; s < 2; ++s) {
                    if (sheets_[s].pageTurnNeeded(nextPly_)) {
                        sheets_[s].beginPageTurn();
                        sheets_[s].finishPageTurn();
                    }
                    std::vector<anim::PenKey> p = sheets_[s].beginMove(nextPly_, san_[size_t(nextPly_)]);
                    if (s == sheetIdx_) path_ = p;
                }
                writing_ = true;
                syncBoard(nextPly_ + 1);
            }
            float dur = path_.empty() ? 0.0f : path_.back().t;
            float w = t - t0_ - entryStart_;
            if (w < dur) {
                for (auto& s : sheets_) s.setWritingTime(w);
                penTime_ = w;
                return;
            }
            for (auto& s : sheets_) {
                s.setWritingTime(dur);
                s.finishEntry();
            }
            writing_ = false;
            ++nextPly_;
            entryStart_ += dur + 0.6f;
            if (t - t0_ < entryStart_) {
                penTime_ = dur;
                return;
            }
        }
    }

    void runHeader(float t) {
        float w = t - t0_;
        if (headerStep_ == 0) {
            for (int s = 0; s < 2; ++s) {
                std::vector<anim::PenKey> p = sheets_[s].beginHeader(header_);
                if (s == sheetIdx_) path_ = p;
            }
            headerStep_ = 1;
        }
        float dur = path_.empty() ? 0.0f : path_.back().t;
        if (headerStep_ == 1) {
            float tw = std::min(w, dur);
            for (auto& s : sheets_) s.setWritingTime(tw);
            penTime_ = tw;
            if (w > dur + 2.0f) {
                for (auto& s : sheets_) s.finishEntry();
                headerStep_ = 2;
            }
        }
        if (w > dur + 4.0f) {
            restart();
            t0_ = t;
        }
    }

    void runTurn(float t) {
        game::Scoresheet& s = sheets_[sheetIdx_];
        if (fixedS_ >= 0.0f) {
            if (s.turnsPending() == 0 && s.currentPage() == 0) s.beginPageTurn();
            s.setTurnProgress(fixedS_);
            return;
        }
        const float period = 4.0f, start = 0.6f, len = 1.4f;
        float w = std::fmod(t - t0_, period);
        if (t - t0_ >= period) {
            t0_ += period * std::floor((t - t0_) / period);
            restart();
        }
        if (w < start) return;
        if (!turnDone_) {
            if (s.turnsPending() == 0) s.beginPageTurn();
            float sp = std::min(1.0f, (w - start) / len);
            s.setTurnProgress(sp);
            if (sp >= 1.0f) {
                s.finishPageTurn();
                turnDone_ = true;
            }
        }
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0.0f && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        cam_.update(plat::input());
        const plat::Input& in = plat::input();
        if (in.keyPressed[plat::KEY_SPACE]) {
            restart();
            t0_ = time_;
        }
        if (demo_ == "write") runWrite(time_);
        else if (demo_ == "header") runHeader(time_);
        else if (demo_ == "turn") runTurn(time_);
        for (auto& s : sheets_) s.update();
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    // Pen tip along the path at time w (world).
    vec3 tipAt(float w) const {
        if (path_.empty()) return vec3(0.0f);
        if (w <= path_.front().t) return path_.front().tip;
        for (size_t k = 1; k < path_.size(); ++k)
            if (w <= path_[k].t) {
                float u = (w - path_[k - 1].t) / std::max(path_[k].t - path_[k - 1].t, 1e-6f);
                return path_[k - 1].tip + (path_[k].tip - path_[k - 1].tip) * u;
            }
        return path_.back().tip;
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env = world_.environment(time_);
        vec3 sun;
        std::string sunArg = ctx.argValue("--sun");
        if (!sunArg.empty() && std::sscanf(sunArg.c_str(), "%f,%f,%f", &sun.x, &sun.y, &sun.z) == 3) env.sunDirection = normalize(sun);
        render::Camera cam = cam_.camera();
        if (view_ == "player") {
            const game::sheet::PadFrame& f = sheets_[sheetIdx_].frame();
            float sz = f.owner == 0 ? 1.0f : -1.0f;
            cam.position = vec3(0.0f, layout::EYE_HEIGHT, sz * layout::PLAYER_PELVIS_Z);
            cam.fovY = 50.0f * DEG;
            cam.nearZ = 0.02f;
            cam.lookAt(f.center + vec3(0.0f, 0.0f, -sz * 0.03f));
        }
        cam.nearZ = std::min(cam.nearZ, 0.01f);
        r.post().settings.dofFocusDistance = length(cam.position - cam_.target);
        r.beginFrame(cam, env, dt);
        world_.submitStatic(r);
        board_.beginFrame();
        board_.updateRestingTransforms();
        world_.submitPieces(r, board_);
        game::ClockDisplay cd;
        cd.ms[0] = 1000 * 60 * 42 + 17000;
        cd.ms[1] = 1000 * 60 * 51 + 3000;
        cd.running = nextPly_ % 2;
        world_.submitClock(r, cd);
        uint32_t id = 60000;
        for (int s = 0; s < 2; ++s) {
            sheets_[s].submit(r, id);
            id += 16;
            bool floating = s == sheetIdx_ && (demo_ == "write" || demo_ == "header") && !path_.empty();
            mat4 pen = floating ? game::sheet::penWritingTransform(sheets_[s].frame(), tipAt(penTime_))
                                : sheets_[s].penRestTransform();
            game::Scoresheet::submitPen(r, pen, id);
            id += 2;
        }
        r.endFrame();
    }

    void shutdown(AppContext&) override {
        for (auto& s : sheets_) s.shutdown();
    }

private:
    AppContext* ctx_ = nullptr;
    game::World world_;
    game::PhysicalBoard board_;
    chess::Game game_;
    std::vector<std::string> san_;
    game::Scoresheet sheets_[2];
    game::Scoresheet::Header header_;
    OrbitCamera cam_;
    std::string demo_, view_;
    int sheetIdx_ = 0;
    bool clockPosX_ = true;
    float fixedS_ = -1.0f;
    int baseMoves_ = 6;
    int nextPly_ = 0;
    float time_ = 0.0f, t0_ = 0.0f, entryStart_ = 0.0f, penTime_ = 0.0f;
    bool writing_ = false, turnDone_ = false;
    int headerStep_ = 0;
    std::vector<anim::PenKey> path_;
};

}  // namespace

SCACELITH_SCENE("scoresheet",
                "Scoresheet pads, handwriting reveal, page turn and pen (--demo write|turn|header|static, --view "
                "close|pad|header|macro|pen|corner|stack|player|overview)",
                ScoresheetScene);
