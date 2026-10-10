// Stances of the robot (see animator.h: setStance): placeholder, the robot stays seated.
#include "animator_impl.h"

namespace anim {

void Animator::setStance(Stance target) { impl_->stanceTarget = target; }
Stance Animator::stanceTarget() const { return impl_->stanceTarget; }
Stance Animator::stance() const { return Stance::Seated; }
bool Animator::seated() const { return true; }
bool Animator::stanceMoving() const { return false; }
float Animator::chairSlide() const { return 0.0f; }

}  // namespace anim
