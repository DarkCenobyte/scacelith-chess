// Online play in the UI (ui_screens_online*.cpp): the menu pages live inside ui::mainMenu()
// (title entry "Play Online", Options > Online); the game uses the in-game pieces below. The
// pages read and drive game::onlineSession() directly.
#pragma once
#include "ui.h"
#include <string>

namespace ui {

// Ping indicator, top right: "32 ms" and a dot (green under 80 ms, amber under 200, red above);
// a grey dash while reconnecting or when unknown (ms < 0). Online menus and online games only.
void pingIndicator(int ms, bool reconnecting);

// Overlay of an online game.
struct OnlineHud {
    int pingMs = -1;
    bool reconnecting = false;     // our connection is being restored: veil and "Reconnecting…"
    std::string countdown;         // first move timer ("Your first move · 0:24"), "" = none
    std::string banner;            // "Opponent disconnected — 0:45 to return", "" = none
    bool drawOffer = false;        // the opponent offers a draw: card with Accept / Decline
};
enum class OnlineHudAction { None, AcceptDraw, DeclineDraw };
OnlineHudAction onlineHud(const OnlineHud& hud);

// Esc menu of an online game: Resume, Offer draw, Claim draw, Abort (before your first move),
// Resign, Report opponent (server games), Options, Leave (= resign, confirmed). Returns Resume,
// OfferDraw, ClaimDraw, Abort, Resign, Report, OptionsChanged or BackToMainMenu (leave). Esc
// resumes.
struct OnlinePause {
    bool canOfferDraw = true, canClaimDraw = false, canAbort = false, canReport = true;
};
MenuAction onlinePauseMenu(const OnlinePause& p);

// Report the opponent: category 0 cheating, 1 abusive behaviour, 2 other; optional comment.
// Returns -1 while open, 1 = send, 0 = cancelled.
int reportDialog(int& category, std::string& comment);

// Cards of the challenges received (from, rating, time control, rated; Accept / Decline):
// shown by the online pages, and at the table between two online games.
void onlineChallenges();

}  // namespace ui
