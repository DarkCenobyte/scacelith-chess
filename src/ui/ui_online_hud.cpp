// Online play at the table and over the menus: the ping indicator, the overlay of an online game
// (first-move timer, "opponent disconnected" banner, the opponent's draw offer, our own
// reconnection veil), the Esc menu of an online game, the report dialog and the cards of the
// challenges received. Same look as ui_screens.cpp.
#include "ui_online.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../game/online_session.h"
#include "../i18n/i18n.h"
#include <algorithm>
#include <cmath>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;

namespace {

TextStyle style(int face, float size, vec4 color, HAlign align = HAlign::Left, float tracking = 0.0f) {
    TextStyle st;
    st.face = face;
    st.size = size;
    st.color = color;
    st.align = align;
    st.tracking = tracking;
    return st;
}
float ease(float t) { return m::smootherstep(t); }
std::string T(const char* key) { return i18n::tr(key); }
std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }
vec2 view() { return gfx::viewSize(); }
Rect screenRect() {
    vec2 v = view();
    return Rect(0, 0, v.x, v.y);
}

const vec4 kGood(0.47f, 0.76f, 0.43f, 1.0f), kFair(0.90f, 0.70f, 0.30f, 1.0f), kPoor(0.86f, 0.36f, 0.30f, 1.0f);

// A dark band like the toasts (fades out at both ends).
void band(const Rect& r, float a) {
    float edge = 0.25f;
    vec4 d(0.02f, 0.017f, 0.015f, 0.78f * a), z(0.02f, 0.017f, 0.015f, 0.0f);
    gfx::fillH(Rect(r.x, r.y, r.w * edge, r.h), z, d);
    gfx::fill(Rect(r.x + r.w * edge, r.y, r.w * (1.0f - 2.0f * edge), r.h), d);
    gfx::fillH(Rect(r.r() - r.w * edge, r.y, r.w * edge, r.h), d, z);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.y, withAlpha(gold, 0.55f * a), 0.45f);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.b() - 1.0f, withAlpha(gold, 0.55f * a), 0.45f);
}

void spinner(vec2 c, float r, float alpha) {
    float t = float(im::time());
    for (int i = 0; i < 8; ++i) {
        float a = float(i) / 8.0f * 6.2831853f;
        float phase = std::fmod(t * 1.3f - float(i) / 8.0f + 8.0f, 1.0f);
        gfx::diamond(vec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), 2.8f, withAlpha(gold, (0.2f + 0.8f * phase) * alpha));
    }
}

// Pause menu state.
struct PauseState {
    bool options = false;
    int confirm = 0;  // 1 resign, 2 leave
};
PauseState P;

}  // namespace

// ---- Ping ----------------------------------------------------------------------------------------------
void pingIndicator(int ms, bool reconnecting) {
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    vec2 v = view();
    bool known = !reconnecting && ms >= 0;
    vec4 dot = !known ? withAlpha(muted, 0.8f) : ms < 80 ? kGood : ms < 200 ? kFair : kPoor;
    std::string text = known ? i18n::trf("online.ping", {std::to_string(ms)}) : std::string("\xE2\x80\x94");
    TextStyle ts = style(font::FACE_TEXT, kCaption, withAlpha(ivoryDim, 0.9f), HAlign::Right);
    float tw = gfx::textWidth(text, ts);
    float right = v.x - 34.0f, base = 46.0f;
    Rect bg(right - tw - 42.0f, base - 26.0f, tw + 56.0f, 38.0f);
    gfx::fill(bg, vec4(0.0f, 0.0f, 0.0f, 0.35f), 19.0f);
    gfx::text(text, right, base, ts);
    vec2 c(right - tw - 20.0f, base - 7.0f);
    gfx::circle(c, 6.0f, withAlpha(dot, 0.25f));
    gfx::circle(c, 4.0f, dot);
    gfx::setLayer(prev);
}

// ---- Game overlay --------------------------------------------------------------------------------------
OnlineHudAction onlineHud(const OnlineHud& hud) {
    OnlineHudAction act = OnlineHudAction::None;
    vec2 v = view();
    pingIndicator(hud.pingMs, hud.reconnecting);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    // Bottom center: the opponent's absence, then the first-move timer.
    float by = v.y - 130.0f;
    im::Anim& ba = im::anim(im::makeId("##online.banner"));
    ba.v[0] = im::approach(ba.v[0], hud.banner.empty() ? 0.0f : 1.0f, 8.0f);
    static std::string lastBanner;  // kept while the banner fades out
    if (!hud.banner.empty()) lastBanner = hud.banner;
    if (ba.v[0] > 0.01f && !lastBanner.empty()) {
        TextStyle ts = style(font::FACE_ITALIC, 26.0f, withAlpha(ivory, ba.v[0]), HAlign::Center);
        float w = gfx::textWidth(lastBanner, ts) + 260.0f;
        Rect r(v.x * 0.5f - w * 0.5f, by, w, 56.0f);
        band(r, ba.v[0]);
        gfx::text(lastBanner, r.cx(), r.cy() + 8.0f, ts);
        by -= 70.0f;
    }
    if (!hud.countdown.empty()) {
        TextStyle ts = style(font::FACE_TITLE, 20.0f, withAlpha(goldBright, 0.95f), HAlign::Center, 0.14f);
        float w = gfx::textWidth(hud.countdown, ts) + 200.0f;
        Rect r(v.x * 0.5f - w * 0.5f, v.y - 72.0f, w, 44.0f);
        band(r, 0.8f);
        gfx::text(hud.countdown, r.cx(), r.cy() + 7.0f, ts);
    }
    // The opponent offers a draw: a card on the end side, mouse buttons (Space belongs to the game).
    im::Anim& da = im::anim(im::makeId("##online.drawcard"));
    da.v[0] = im::approach(da.v[0], hud.drawOffer ? 1.0f : 0.0f, 10.0f);
    if (da.v[0] > 0.01f) {
        float t = ease(da.v[0]);
        float w = 440.0f, h = 176.0f;
        Rect r = im::flip(screenRect(), Rect(v.x - w - 40.0f + (1.0f - t) * 30.0f, v.y * 0.5f - h * 0.5f + 60.0f, w, h));
        im::captureMouseRect(r);
        gfx::pushAlpha(t);
        if (!hud.drawOffer) im::pushBlock();
        im::panel(r);
        TextStyle cap = style(font::FACE_TITLE, 19.0f, gold, HAlign::Center, 0.2f);
        gfx::text(T("online.draw.card_title"), r.cx(), r.y + 46.0f, cap);
        TextStyle ts = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
        ts.size = gfx::fitSize(T("online.draw.card_text"), ts, w - 40.0f, 0.75f);
        gfx::text(T("online.draw.card_text"), r.cx(), r.y + 82.0f, ts);
        float bw = 170.0f, bh = 48.0f, gap = 20.0f;
        float y = r.b() - 26.0f - bh;
        im::pushId("drawcard");
        if (im::button(L("online.draw.decline"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, y, bw, bh)), im::ButtonKind::Secondary, true,
                       im::ITEM_MOUSE_ONLY))
            act = OnlineHudAction::DeclineDraw;
        if (im::button(L("online.draw.accept"), im::flip(r, Rect(r.cx() + gap * 0.5f, y, bw, bh)), im::ButtonKind::Primary, true,
                       im::ITEM_MOUSE_ONLY))
            act = OnlineHudAction::AcceptDraw;
        im::popId();
        if (!hud.drawOffer) im::popBlock();
        gfx::popAlpha();
    }
    gfx::setLayer(prev);
    // Our connection is being restored: a veil over everything.
    im::Anim& ra = im::anim(im::makeId("##online.reconnecting"));
    ra.v[0] = im::approach(ra.v[0], hud.reconnecting ? 1.0f : 0.0f, hud.reconnecting ? 3.0f : 8.0f);
    if (ra.v[0] > 0.01f) {
        float a = ra.v[0];
        gfx::setLayer(gfx::LAYER_MODAL);
        gfx::fill(Rect(0, 0, v.x, v.y), vec4(0, 0, 0, 0.45f * a));
        TextStyle ts = style(font::FACE_TITLE, 34.0f, withAlpha(ivory, a), HAlign::Center, 0.2f);
        gfx::text(T("online.reconnecting"), v.x * 0.5f, v.y * 0.5f - 10.0f, ts);
        TextStyle ss = style(font::FACE_ITALIC, kBody, withAlpha(ivoryDim, a), HAlign::Center);
        gfx::textWrapped(T("online.reconnecting.text"), v.x * 0.5f, v.y * 0.5f + 44.0f, 760.0f, ss, 36.0f);
        spinner(vec2(v.x * 0.5f, v.y * 0.5f - 80.0f), 18.0f, a);
        gfx::setLayer(prev);
    }
    return act;
}

// ---- Esc menu ------------------------------------------------------------------------------------------
MenuAction onlinePauseMenu(const OnlinePause& op) {
    im::Id id = im::makeId("##onlinepause");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    if (appear) {
        P = PauseState();
        im::sound(Sound::Open);
    }
    im::captureMouseAll();
    im::captureKeyboard();
    MenuAction act = MenuAction::None;
    if (P.options) {
        if (detail::runOptionsPage(act)) P.options = false;
        return act;
    }
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.3f);
    float t = ease(a.v[5]);
    vec2 v = view();
    detail::dimBackground(std::max(t, 0.001f));
    int entries = 6 + (op.canReport ? 1 : 0);
    float step = 66.0f, eh = 56.0f;
    float w = 580.0f, h = 210.0f + step * float(entries) + 20.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    if (P.confirm) im::pushBlock();
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("pause.title"), p.cx(), p.y + 80.0f);
    TextStyle ns = style(font::FACE_ITALIC, kCaption, muted, HAlign::Center);
    gfx::text(T("online.pause.clock_runs"), p.cx(), p.y + 140.0f, ns);
    im::pushId("onlinepause");
    Rect er(p.x + 40.0f, p.y + 170.0f, p.w - 80.0f, eh);
    int i = 0;
    auto next = [&]() { return er.offset(0, step * float(i++)); };
    im::Id resumeId = im::makeId("##pause.resume");
    if (im::menuEntry(L("pause.resume"), next(), true, HAlign::Center)) act = MenuAction::Resume;
    if (im::menuEntry(L("pause.offer_draw"), next(), op.canOfferDraw, HAlign::Center)) act = MenuAction::OfferDraw;
    if (im::menuEntry(L("pause.claim_draw"), next(), op.canClaimDraw, HAlign::Center)) act = MenuAction::ClaimDraw;
    im::tooltip(T("pause.claim_draw.help"));
    if (op.canAbort) {
        if (im::menuEntry(L("online.pause.abort"), next(), true, HAlign::Center)) act = MenuAction::Abort;
        im::tooltip(T("online.pause.abort.help"));
    } else {
        if (im::menuEntry(L("pause.resign"), next(), true, HAlign::Center)) P.confirm = 1;
    }
    if (op.canReport && im::menuEntry(L("online.report.button"), next(), true, HAlign::Center)) act = MenuAction::Report;
    if (im::menuEntry(L("menu.options"), next(), true, HAlign::Center)) {
        P.options = true;
        detail::openOptionsPage();
    }
    if (im::menuEntry(L("online.pause.leave"), next(), true, HAlign::Center)) P.confirm = 2;
    im::setDefaultFocus(resumeId);
    im::popId();
    gfx::popAlpha();
    if (P.confirm) im::popBlock();
    if (!P.confirm && !appear && im::consumeBack()) {
        act = MenuAction::Resume;
        im::sound(Sound::Close);
    }
    if (P.confirm == 1) {
        int r = im::confirmDialog("##onlineresign", T("confirm.resign.title"), T("online.confirm.resign.text"), T("confirm.resign.ok"),
                                  T("common.cancel"), true);
        if (r == 1) act = MenuAction::Resign;
        if (r >= 0) P.confirm = 0;
    } else if (P.confirm == 2) {
        int r = im::confirmDialog("##onlineleave", T("confirm.leave.title"),
                                  T(op.canAbort ? "online.confirm.leave_abort.text" : "online.confirm.leave.text"),
                                  T(op.canAbort ? "online.confirm.leave_abort.ok" : "online.confirm.leave.ok"), T("common.cancel"), true);
        if (r == 1) act = MenuAction::BackToMainMenu;
        if (r >= 0) P.confirm = 0;
    }
    return act;
}

// ---- Report ----------------------------------------------------------------------------------------------
int reportDialog(int& category, std::string& comment) {
    im::Id id = im::makeId("##onlinereport");
    im::Anim& a = im::anim(id);
    bool first = a.firstFrame == im::frame();
    a.v[0] = first ? 0.0f : im::approach(a.v[0], 1.0f, 12.0f);
    float t = a.v[0];
    im::captureMouseAll();
    im::captureKeyboard();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MODAL);
    vec2 v = view();
    gfx::fill(Rect(0, 0, v.x, v.y), vec4(0, 0, 0, 0.55f * t));
    gfx::pushAlpha(t);
    float w = 860.0f, h = 520.0f;
    Rect r(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    gfx::fill(r, vec4(0.035f, 0.03f, 0.027f, 1.0f), 3.0f);
    im::panel(r);
    im::pageTitle(T("online.report.title"), r.cx(), r.y + 72.0f);
    TextStyle ls = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
    gfx::textWrapped(T("online.report.lead"), r.cx(), r.y + 136.0f, w - 140.0f, ls, 29.0f);
    im::pushId("report");
    category = std::clamp(category, 0, 2);
    im::selectorRow(L("online.report.category"), category,
                    {T("online.report.cheating"), T("online.report.abuse"), T("online.report.other")}, Rect(r.x + 60.0f, r.y + 210.0f, w - 120.0f, 56.0f));
    im::formField(L("online.report.comment"), comment, Rect(r.x + 60.0f, r.y + 276.0f, w - 120.0f, 56.0f), 280, 0u,
                  T("online.report.comment.hint"));
    float bw = 250.0f, bh = 56.0f, gap = 30.0f, by = r.b() - 44.0f - bh;
    int result = -1;
    im::Id cancelId = im::makeId("##common.cancel");
    if (first) im::setFocus(cancelId);
    im::setDefaultFocus(cancelId);
    if (im::button(L("common.cancel"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, by, bw, bh)), im::ButtonKind::Secondary)) result = 0;
    if (im::button(L("online.report.send"), im::flip(r, Rect(r.cx() + gap * 0.5f, by, bw, bh)), im::ButtonKind::Primary)) result = 1;
    im::popId();
    if (!first && result < 0 && !im::editingText() && im::consumeBack()) {
        result = 0;
        im::sound(Sound::Back);
    }
    if (result == 1) im::sound(Sound::Confirm);
    gfx::popAlpha();
    gfx::setLayer(prev);
    if (result >= 0) a.v[0] = 0.0f;
    return result;
}

// ---- Challenges received -------------------------------------------------------------------------------
void onlineChallenges() {
    game::OnlineSession& s = game::onlineSession();
    if (s.incoming().empty()) return;
    std::vector<game::OnlineSession::Incoming> list = s.incoming();  // answering edits the session's list
    vec2 v = view();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    double now = s.nowMs();
    float w = 420.0f, h = 200.0f;
    float y = v.y - 32.0f - h;
    int shown = 0;
    for (auto it = list.rbegin(); it != list.rend() && shown < 3; ++it) {
        const game::OnlineSession::Incoming& c = *it;
        double left = c.expiresMs - now;
        if (left <= 0.0) continue;
        ++shown;
        Rect r = im::flip(screenRect(), Rect(v.x - w - 32.0f, y, w, h));
        y -= h + 14.0f;
        im::captureMouseRect(r);
        im::Anim& a = im::anim(im::makeId(int(c.id) + 100000));
        a.v[0] = im::approach(a.v[0], 1.0f, 9.0f);
        float t = ease(a.v[0]);
        gfx::pushAlpha(t);
        Rect rr = r.offset((im::rtl() ? -1.0f : 1.0f) * (1.0f - t) * 40.0f, 0.0f);
        im::panel(rr);
        TextStyle cap = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.2f);
        gfx::text(T("online.challenge.received"), im::flipX(rr, rr.x + 26.0f), rr.y + 38.0f, cap);
        TextStyle tl = style(font::FACE_TEXT, kCaption, muted, im::endAlign());
        gfx::text(game::durationText(left), im::flipX(rr, rr.r() - 26.0f), rr.y + 38.0f, tl);
        std::string who = c.from.name;
        if (c.from.rating > 0) who += "  " + i18n::ltr(std::to_string(c.from.rating) + (c.from.provisional ? "?" : ""));
        TextStyle ns = style(font::FACE_TEXT, 30.0f, ivory, im::startAlign());
        ns.size = gfx::fitSize(who, ns, w - 52.0f, 0.7f);
        gfx::text(who, im::flipX(rr, rr.x + 26.0f), rr.y + 80.0f, ns);
        std::string tc = std::to_string(c.baseSec / 60) + "\xE2\x80\x89+\xE2\x80\x89" + std::to_string(c.incSec);
        if (c.baseSec % 60 != 0) tc = std::to_string(c.baseSec) + "s\xE2\x80\x89+\xE2\x80\x89" + std::to_string(c.incSec);
        std::string terms = i18n::ltr(tc) + "  \xC2\xB7  " + T(c.rated ? "online.rated" : "online.casual") + "  \xC2\xB7  " +
                            T(c.yourColor == 1 ? "online.challenge.you_white" : c.yourColor == 2 ? "online.challenge.you_black"
                                                                                                 : "online.challenge.you_random");
        TextStyle ts = style(font::FACE_ITALIC, kCaption, ivoryDim, im::startAlign());
        ts.size = gfx::fitSize(terms, ts, w - 52.0f, 0.7f);
        gfx::text(terms, im::flipX(rr, rr.x + 26.0f), rr.y + 112.0f, ts);
        float bw = (w - 52.0f - 14.0f) * 0.5f, bh = 46.0f, by = rr.b() - 22.0f - bh;
        im::pushId(int(c.id));
        bool decline = im::button(L("online.challenge.decline"), im::flip(rr, Rect(rr.x + 26.0f, by, bw, bh)), im::ButtonKind::Secondary, true,
                                  im::ITEM_MOUSE_ONLY);
        bool accept = im::button(L("online.challenge.accept"), im::flip(rr, Rect(rr.x + 26.0f + bw + 14.0f, by, bw, bh)), im::ButtonKind::Primary,
                                 true, im::ITEM_MOUSE_ONLY);
        im::popId();
        gfx::popAlpha();
        if (accept) s.answerChallenge(c.id, true);
        else if (decline) s.answerChallenge(c.id, false);
    }
    gfx::setLayer(prev);
}

}  // namespace ui
