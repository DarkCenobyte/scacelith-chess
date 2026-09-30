// Placeholder implementation (silent coach) until the session lands; see session.h.
#include "session.h"

namespace coach {

struct Session::Impl {
    SessionConfig config;
    Director director;
    std::vector<GameRecord> history;
    bool over = false;
};

Session::Session() : d_(new Impl) {}
Session::~Session() { delete d_; }
void Session::start(Stage& stage, Analyst&, const chess::Game&, const SessionConfig& config) {
    d_->config = config;
    d_->history = config.history;
    d_->over = false;
    d_->director.reset(&stage, config.director);
}
void Session::stop() { d_->director.clear(); }
const SessionConfig& Session::config() const { return d_->config; }
void Session::update(const chess::Game& game, float dt) { d_->director.update(dt, int(game.moves().size())); }
void Session::setPaused(bool paused) { d_->director.setPaused(paused); }
void Session::onMove(const chess::Game&) {}
void Session::onPlayerActive() {}
void Session::onIllegalAttempt(const chess::Game&, chess::Square, chess::Square) {}
void Session::onOfferAnswer(const chess::Game&, bool) {}
bool Session::canTakeBack(const chess::Game&) const { return false; }
void Session::onTakeBackRequested(const chess::Game&) {}
void Session::onTakenBack(const chess::Game&) {}
void Session::onDrawAnswer(bool) {}
void Session::onGameOver(const chess::Game&, bool) { d_->over = true; }
void Session::skip() {}
bool Session::coachMayMove() const { return true; }
bool Session::handshakeWanted() const { return d_->over; }
void Session::onHandshakeDone(const chess::Game&) {}
bool Session::finished() const { return d_->over; }
bool Session::offerOpen() const { return false; }
const std::vector<GameRecord>& Session::history() const { return d_->history; }
bool Session::accuracyExplained() const { return d_->config.accuracyExplained; }
int Session::suggestedLevel() const { return 0; }
int Session::lessonChapter() const { return d_->config.lessonChapter; }
bool Session::lessonCompleted() const { return false; }
Director& Session::director() { return d_->director; }
const Director& Session::director() const { return d_->director; }

}  // namespace coach
