// Writing-hand API: placeholder until the animation package implements it (the pen stays on the
// table, nothing is written).
#include "animator.h"

namespace anim {

float writeTaskDuration(const WriteTask& t) {
    switch (t.type) {
    case WriteTaskType::PickPen: return Timing::PickPen;
    case WriteTaskType::PutPen: return Timing::PutPen;
    case WriteTaskType::TurnPage: return t.duration > 0.0f ? t.duration : Timing::PageTurn;
    case WriteTaskType::Write:
        return Timing::WriteApproach + (t.path.empty() ? 0.0f : t.path.back().t) + Timing::WriteRetract;
    default: return t.duration;
    }
}

character::Side Animator::playHand() const { return character::Side::Right; }
character::Side Animator::writingHand() const { return character::Side::Left; }
void Animator::setWritingRest(m::vec3) {}
void Animator::enqueueWriting(const WriteTask&) {}
void Animator::enqueueWriting(const std::vector<WriteTask>&) {}
bool Animator::writingBusy() const { return false; }
float Animator::writingPathTime() const { return -1.0f; }
float Animator::pageTurnProgress() const { return -1.0f; }
bool Animator::penTransform(m::mat4&) const { return false; }
bool Animator::holdsPen() const { return false; }

}  // namespace anim
