// Placeholder implementation (silent coach) until the director lands; see director.h.
#include "director.h"

namespace coach {

struct Director::Impl {
    Stage* stage = nullptr;
    DirectorConfig config;
    std::vector<ShownMark> marks;
};

Director::Director() : d_(new Impl) {}
Director::~Director() { delete d_; }
void Director::reset(Stage* stage, const DirectorConfig& config) { d_->stage = stage; d_->config = config; }
void Director::setConfig(const DirectorConfig& config) { d_->config = config; }
const DirectorConfig& Director::config() const { return d_->config; }
void Director::play(const Script&) {}
void Director::playNext(const Script&) {}
void Director::prefetch(const std::vector<Line>&) {}
void Director::update(float, int) {}
void Director::setPaused(bool) {}
void Director::skip() {}
void Director::playerActed() {}
void Director::clear() {}
bool Director::idle() const { return true; }
bool Director::speaking() const { return false; }
bool Director::skippable() const { return false; }
bool Director::waitingMove(int*) const { return false; }
void Director::endWait() {}
bool Director::offerOpen() const { return false; }
void Director::closeOffer() {}
const std::vector<ShownMark>& Director::marks() const { return d_->marks; }

}  // namespace coach
