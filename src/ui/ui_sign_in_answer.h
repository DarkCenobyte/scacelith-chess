// Internal to src/ui: what a refused sign-in answer (a LoginResult that is not ok) does on the
// pages of signing in (ui_screens_online.cpp). Header-only, for the unit tests.
#pragma once
#include <string>

namespace ui {
namespace detail {

enum class RefusedSignIn {
    Code,         // the code step (two-factor): to the code page
    Silent,       // a Google sign-in the player stopped: nothing to tell, whatever the page
    StayOnLink,   // the password step of adding Google sign-in stays for another try
    Other,        // the error, on the page it sends the player back to
};

// onLinkPage: the answer reaches the password step of adding Google sign-in (the server keeps
// that step after a wrong password or a pause it asks for, until the fifth miss).
inline RefusedSignIn refusedSignIn(bool onLinkPage, bool mfaRequired, const std::string& error) {
    if (mfaRequired) return RefusedSignIn::Code;
    if (error == "cancelled") return RefusedSignIn::Silent;
    if (onLinkPage && (error == "invalid_credentials" || error == "too_many_attempts")) return RefusedSignIn::StayOnLink;
    return RefusedSignIn::Other;
}

}  // namespace detail
}  // namespace ui
